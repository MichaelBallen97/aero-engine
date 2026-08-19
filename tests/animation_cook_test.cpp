// tests/animation_cook_test.cpp -- task 3.5.2: cookAnimation, the resolved-channels-to-container
// transform. A TU of aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every case drives the PUBLIC cookAnimation() and reads its
// output back through the PUBLIC parseCookedAnimation(), so nothing here depends on an internal of
// either. The two golden INPUTS live here (minimalChannels/mixedChannels) and their frozen BYTES
// live in cooked_animation_golden.hpp -- one array, two test binaries, no drift.
#include <aero/assets/animation_cook.hpp>
#include <aero/assets/cooked_animation.hpp>

#include "cooked_animation_golden.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies std::string_view operands
// through operator<<(std::ostream&, std::string_view), which MS STL defines inline in <string_view>
// against an INCOMPLETE std::basic_ostream. libc++ and libstdc++ are self-sufficient, so omitting it
// builds clean on macOS and Linux and fails only on the Windows lane. Written when the TU was
// created, not after a lane said so.
#include <ostream>
#include <utility>
#include <vector>

using engine::Guid;
using engine::Vec4;
using engine::assets::AnimationCookChannel;
using engine::assets::AnimationCookInput;
using engine::assets::AnimationCookResult;
using engine::assets::AnimationCookStatus;
using engine::assets::animationKeyTime;
using engine::assets::animationKeyValue;
using engine::assets::channelTimeBytes;
using engine::assets::channelValueBytes;
using engine::assets::cookAnimation;
using engine::assets::CookedAnimationInterpolation;
using engine::assets::CookedAnimationParseResult;
using engine::assets::CookedAnimationPath;
using engine::assets::CookedAnimationStatus;
using engine::assets::cookedAnimationTimesPadding;
using engine::assets::MAX_COOK_WARNINGS;
using engine::assets::MAX_COOKED_ANIMATION_CHANNELS;
using engine::assets::MAX_COOKED_ANIMATION_KEYS;
using engine::assets::MAX_COOKED_ANIMATION_VALUES;
using engine::assets::parseCookedAnimation;

namespace {

// Backing storage for hand-built channels: the two spans an AnimationCookChannel carries must
// outlive the cook call. A deque rather than a vector, so a later add() can never move the arrays an
// earlier span already points at.
class ClipBuilder {
public:
    void add(std::uint32_t node, CookedAnimationPath path, CookedAnimationInterpolation interpolation,
             std::vector<float> times, std::vector<Vec4> values) {
        timeStore.push_back(std::move(times));
        valueStore.push_back(std::move(values));
        AnimationCookChannel channel;
        channel.targetLocalId = node;
        channel.path = path;
        channel.interpolation = interpolation;
        channel.times = std::span<const float>(timeStore.back());
        channel.values = std::span<const Vec4>(valueStore.back());
        list.push_back(channel);
    }
    // A channel whose path or interpolation code this format does not define -- reachable only by a
    // static_cast, which is exactly the caller bug the cook's own validation exists for.
    void addRawCodes(std::uint32_t node, std::uint16_t path, std::uint16_t interpolation, std::vector<float> times,
                     std::vector<Vec4> values) {
        add(node, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, std::move(times),
            std::move(values));
        list.back().path = static_cast<CookedAnimationPath>(path);
        list.back().interpolation = static_cast<CookedAnimationInterpolation>(interpolation);
    }
    [[nodiscard]] std::span<const AnimationCookChannel> channels() const {
        return std::span<const AnimationCookChannel>(list);
    }

private:
    std::deque<std::vector<float>> timeStore;
    std::deque<std::vector<Vec4>> valueStore;
    std::vector<AnimationCookChannel> list;
};

// ---- the two golden INPUTS ----------------------------------------------------------------------
// MINIMAL: one Linear Rotation channel on node 5, two keys, a nil GUID and source animation index 0.
// The smallest legal file that still exercises the format's single padding site (keyCount % 4 == 2,
// so the site is eight bytes wide). Every float is exactly representable in binary32.
struct MinimalClipData {
    std::array<float, 2> times{0.0F, 0.5F};
    std::array<Vec4, 2> values{Vec4{0.0F, 0.0F, 0.0F, 1.0F}, Vec4{0.0F, 0.0F, 1.0F, 0.0F}};
};

[[nodiscard]] std::array<AnimationCookChannel, 1> minimalChannels(const MinimalClipData& data) {
    AnimationCookChannel rotation;
    rotation.targetLocalId = 5;
    rotation.path = CookedAnimationPath::Rotation;
    rotation.interpolation = CookedAnimationInterpolation::Linear;
    rotation.times = std::span<const float>(data.times);
    rotation.values = std::span<const Vec4>(data.values);
    return {rotation};
}

// MIXED: three channels, one per interpolation mode, TWO OF THEM ON THE SAME NODE with different
// paths, supplied in an order that is neither the emitted order nor the order a node-only sort would
// produce. A real GUID and source animation index 2. Its total key count is odd, so the padding site
// is present and four bytes wide; its Translation values carry a caller-supplied non-zero w, so the
// forcing rule is pinned by bytes rather than by a comment.
struct MixedClipData {
    // input slot 0 -- node 7, Scale, CubicSpline: inTangent, value, outTangent per key.
    std::array<float, 2> scaleTimes{0.0F, 2.0F};
    std::array<Vec4, 6> scaleValues{Vec4{0.0F, 0.0F, 0.0F, 0.0F},    Vec4{1.0F, 1.0F, 1.0F, 0.0F},
                                    Vec4{0.25F, 0.25F, 0.25F, 0.0F}, Vec4{0.5F, 0.5F, 0.5F, 0.0F},
                                    Vec4{2.0F, 2.0F, 2.0F, 0.0F},    Vec4{0.0F, 0.0F, 0.0F, 0.0F}};
    // input slot 1 -- node 3, Rotation, Linear.
    std::array<float, 3> rotationTimes{0.0F, 0.25F, 1.0F};
    std::array<Vec4, 3> rotationValues{Vec4{0.0F, 0.0F, 0.0F, 1.0F}, Vec4{0.5F, 0.0F, 0.0F, 0.5F},
                                       Vec4{0.0F, 0.0F, 1.0F, 0.0F}};
    // input slot 2 -- node 7, Translation, Step, with a NON-ZERO w the cook must overwrite.
    std::array<float, 2> translationTimes{0.0F, 0.5F};
    std::array<Vec4, 2> translationValues{Vec4{1.0F, 2.0F, 3.0F, 7.0F}, Vec4{4.0F, 5.0F, 6.0F, -8.0F}};
};

inline constexpr Guid MIXED_GUID{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
inline constexpr std::uint32_t MIXED_ANIMATION_INDEX = 2;

[[nodiscard]] std::array<AnimationCookChannel, 3> mixedChannels(const MixedClipData& data) {
    AnimationCookChannel scale;
    scale.targetLocalId = 7;
    scale.path = CookedAnimationPath::Scale;
    scale.interpolation = CookedAnimationInterpolation::CubicSpline;
    scale.times = std::span<const float>(data.scaleTimes);
    scale.values = std::span<const Vec4>(data.scaleValues);

    AnimationCookChannel rotation;
    rotation.targetLocalId = 3;
    rotation.path = CookedAnimationPath::Rotation;
    rotation.interpolation = CookedAnimationInterpolation::Linear;
    rotation.times = std::span<const float>(data.rotationTimes);
    rotation.values = std::span<const Vec4>(data.rotationValues);

    AnimationCookChannel translation;
    translation.targetLocalId = 7;
    translation.path = CookedAnimationPath::Translation;
    translation.interpolation = CookedAnimationInterpolation::Step;
    translation.times = std::span<const float>(data.translationTimes);
    translation.values = std::span<const Vec4>(data.translationValues);

    return {scale, rotation, translation};
}

[[nodiscard]] AnimationCookResult cook(std::span<const AnimationCookChannel> channels, Guid guid = Guid{},
                                       std::uint32_t animationIndex = 0) {
    AnimationCookInput input;
    input.sourceGuid = guid;
    input.sourceAnimationIndex = animationIndex;
    input.channels = channels;
    return cookAnimation(input);
}

[[nodiscard]] CookedAnimationParseResult parse(const std::vector<std::byte>& bytes) {
    return parseCookedAnimation(std::span<const std::byte>(bytes));
}

[[nodiscard]] bool mentions(const std::string& message, std::string_view needle) {
    return message.find(needle) != std::string::npos;
}

[[nodiscard]] std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }

// The offset of the first byte where a cook's output and a frozen golden disagree, or the golden's
// size when they agree everywhere. Reported rather than a bare bool, so a red case names the field.
[[nodiscard]] std::size_t firstDifference(const std::vector<std::byte>& bytes, std::span<const std::uint8_t> golden) {
    const std::size_t shared = std::min(bytes.size(), golden.size());
    for (std::size_t i = 0; i < shared; ++i) {
        if (bytes[i] != static_cast<std::byte>(golden[i])) {
            return i;
        }
    }
    return shared;
}

// A one-key channel, the cheapest legal shape, for the ordering and duplicate cases.
[[nodiscard]] std::vector<float> oneTime(float t) { return {t}; }
[[nodiscard]] std::vector<Vec4> oneValue(float x) { return {Vec4{x, 0.0F, 0.0F, 0.0F}}; }

}  // namespace

TEST_CASE("animation cook: the minimal clip cooks to its frozen golden, byte for byte (KA1)") {
    const MinimalClipData data;
    const std::array<AnimationCookChannel, 1> channels = minimalChannels(data);
    const AnimationCookResult r = cook(std::span<const AnimationCookChannel>(channels));
    REQUIRE((r.status == AnimationCookStatus::Ok));
    CHECK(r.message.empty());
    CHECK(r.warnings.empty());
    CHECK(r.bytes.size() == aero_test::COOKED_ANIMATION_GOLDEN_MINIMAL.size());
    CHECK(firstDifference(r.bytes, aero_test::COOKED_ANIMATION_GOLDEN_MINIMAL) ==
          aero_test::COOKED_ANIMATION_GOLDEN_MINIMAL.size());
}

TEST_CASE("animation cook: the mixed clip cooks to its frozen golden, out of emitted order (KA2)") {
    // The three channels are supplied slot 0 = (7, Scale), slot 1 = (3, Rotation), slot 2 =
    // (7, Translation) -- neither the emitted order nor the order a node-only sort produces. The
    // byte equality is therefore the order-independence proof and the golden at once.
    const MixedClipData data;
    const std::array<AnimationCookChannel, 3> channels = mixedChannels(data);
    const AnimationCookResult r =
        cook(std::span<const AnimationCookChannel>(channels), MIXED_GUID, MIXED_ANIMATION_INDEX);
    REQUIRE((r.status == AnimationCookStatus::Ok));
    CHECK(r.message.empty());
    CHECK(r.bytes.size() == aero_test::COOKED_ANIMATION_GOLDEN_MIXED.size());
    CHECK(firstDifference(r.bytes, aero_test::COOKED_ANIMATION_GOLDEN_MIXED) ==
          aero_test::COOKED_ANIMATION_GOLDEN_MIXED.size());
}

TEST_CASE("animation cook: all six permutations of the mixed clip cook to identical bytes (KA3)") {
    const MixedClipData data;
    const std::array<AnimationCookChannel, 3> channels = mixedChannels(data);
    std::array<std::size_t, 3> order = {0, 1, 2};
    std::size_t permutations = 0;
    do {
        const std::array<AnimationCookChannel, 3> shuffled = {channels[order[0]], channels[order[1]],
                                                              channels[order[2]]};
        const AnimationCookResult r =
            cook(std::span<const AnimationCookChannel>(shuffled), MIXED_GUID, MIXED_ANIMATION_INDEX);
        REQUIRE((r.status == AnimationCookStatus::Ok));
        CHECK(firstDifference(r.bytes, aero_test::COOKED_ANIMATION_GOLDEN_MIXED) ==
              aero_test::COOKED_ANIMATION_GOLDEN_MIXED.size());
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK(permutations == 6);  // 3! -- a literal, never order.size() factorial computed here
}

TEST_CASE("animation cook: the same node with two paths emits in PATH order, not input order (KA4)") {
    // Supplied Scale first, and Translation = 0 < Scale = 2, so the correct emitted order is the
    // other one. A sort key that drops `path` cannot produce it: the two collide on one key, which
    // both loses the ordering and makes the pair look like the duplicate a (node, path) collision
    // would be. This is the property the mixed golden's bytes also carry, pinned here on the PARSED
    // RECORDS so it is not hostage to one array.
    ClipBuilder clip;
    clip.add(7, CookedAnimationPath::Scale, CookedAnimationInterpolation::Linear, oneTime(1.0F), oneValue(9.0F));
    clip.add(7, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(2.0F), oneValue(8.0F));
    const AnimationCookResult r = cook(clip.channels());
    REQUIRE((r.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult p = parse(r.bytes);
    REQUIRE((p.status == CookedAnimationStatus::Ok));
    REQUIRE(p.animation.channels.size() == 2);
    CHECK(p.animation.channels[0].targetNodeLocalId == 7);
    CHECK((p.animation.channels[0].path == CookedAnimationPath::Translation));
    CHECK(p.animation.channels[1].targetNodeLocalId == 7);
    CHECK((p.animation.channels[1].path == CookedAnimationPath::Scale));
}

TEST_CASE("animation cook: an empty channel list is a refusal, not an empty file (KA5)") {
    const AnimationCookResult r = cook(std::span<const AnimationCookChannel>{});
    CHECK((r.status == AnimationCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(mentions(r.message, "channel list is empty"));
}

TEST_CASE("animation cook: a clip whose every channel drops is Invalid, with no bytes at all (KA6)") {
    ClipBuilder clip;
    clip.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {}, {});
    clip.add(2, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, {}, {});
    const AnimationCookResult r = cook(clip.channels());
    CHECK((r.status == AnimationCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(mentions(r.message, "every channel was dropped"));
    // A refusal carries its reason in `message` and nothing else. Every refuse() in this cook
    // returns a FRESH result, so the per-channel drop warnings gathered on the way here are not part
    // of one -- pinned, because the alternative is a result that is Invalid and chatty at once.
    CHECK(r.warnings.empty());
    CHECK(r.warningTotal == 0);
}

TEST_CASE("animation cook: one zero-key channel among three is Truncated and still parses (KA7)") {
    ClipBuilder clip;
    clip.add(4, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(0.0F), oneValue(1.0F));
    clip.add(9, CookedAnimationPath::Scale, CookedAnimationInterpolation::Linear, {}, {});
    clip.add(4, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, oneTime(1.0F), oneValue(2.0F));
    const AnimationCookResult r = cook(clip.channels());
    CHECK((r.status == AnimationCookStatus::Truncated));
    CHECK(!r.message.empty());
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warningTotal == 1);
    CHECK(mentions(r.warnings[0], "channel 1"));  // the INPUT index
    CHECK(mentions(r.warnings[0], "node 9"));     // and its targetLocalId
    const CookedAnimationParseResult p = parse(r.bytes);
    CHECK((p.status == CookedAnimationStatus::Ok));
    CHECK(p.animation.channels.size() == 2);
}

TEST_CASE("animation cook: a duplicate (node, path) pair is a refusal, named by node and path (KA8)") {
    ClipBuilder clip;
    clip.add(6, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, oneTime(0.0F), oneValue(1.0F));
    clip.add(6, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Step, oneTime(1.0F), oneValue(2.0F));
    const AnimationCookResult r = cook(clip.channels());
    CHECK((r.status == AnimationCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(r.message == "two channels target node 6 with path Rotation");
}

TEST_CASE("animation cook: the same node with DIFFERENT paths is not a duplicate (KA9)") {
    ClipBuilder clip;
    clip.add(6, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, oneTime(0.0F), oneValue(1.0F));
    clip.add(6, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(1.0F), oneValue(2.0F));
    clip.add(6, CookedAnimationPath::Scale, CookedAnimationInterpolation::Linear, oneTime(2.0F), oneValue(3.0F));
    const AnimationCookResult r = cook(clip.channels());
    CHECK((r.status == AnimationCookStatus::Ok));
    CHECK(r.message.empty());
    const CookedAnimationParseResult p = parse(r.bytes);
    REQUIRE((p.status == CookedAnimationStatus::Ok));
    CHECK(p.animation.channels.size() == 3);
}

TEST_CASE("animation cook: times that do not strictly increase are refused, both ways (KA10)") {
    ClipBuilder equal;
    equal.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {0.0F, 1.0F, 1.0F},
              {Vec4{}, Vec4{}, Vec4{}});
    const AnimationCookResult re = cook(equal.channels());
    CHECK((re.status == AnimationCookStatus::Invalid));
    CHECK(mentions(re.message, "strictly increasing"));
    CHECK(mentions(re.message, "key 2"));

    ClipBuilder decreasing;
    decreasing.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {0.0F, 2.0F, 1.0F},
                   {Vec4{}, Vec4{}, Vec4{}});
    const AnimationCookResult rd = cook(decreasing.channels());
    CHECK((rd.status == AnimationCookStatus::Invalid));
    CHECK(mentions(rd.message, "strictly increasing"));

    // The anti-vacuity twin: strictly increasing IS accepted, including a negative-to-positive run.
    ClipBuilder rising;
    rising.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {-2.0F, 0.0F, 1.0F},
               {Vec4{}, Vec4{}, Vec4{}});
    CHECK((cook(rising.channels()).status == AnimationCookStatus::Ok));
}

TEST_CASE("animation cook: an undefined code outranks an empty key list, deliberately (KA11)") {
    // A bad CODE is a caller bug and must never be silently dropped, so the triage checks it FIRST --
    // both of these channels would otherwise be dropped with a warning and the clip would refuse for
    // the wrong reason.
    ClipBuilder path;
    path.addRawCodes(1, 3, 0, {}, {});
    const AnimationCookResult rp = cook(path.channels());
    CHECK((rp.status == AnimationCookStatus::Invalid));
    CHECK(mentions(rp.message, "path code 3"));
    CHECK(rp.warningTotal == 0);

    ClipBuilder interpolation;
    interpolation.addRawCodes(1, 0, 3, {}, {});
    const AnimationCookResult ri = cook(interpolation.channels());
    CHECK((ri.status == AnimationCookStatus::Invalid));
    CHECK(mentions(ri.message, "interpolation code 3"));
    CHECK(ri.warningTotal == 0);

    // 65535 is as undefined as 3, and both fields are whole u16s.
    ClipBuilder wide;
    wide.addRawCodes(1, 65535, 0, oneTime(0.0F), oneValue(1.0F));
    CHECK((cook(wide.channels()).status == AnimationCookStatus::Invalid));
}

TEST_CASE("animation cook: values must be keys times THE multiplier, both modes, both ways (KA12)") {
    const std::array<std::size_t, 4> valueCounts = {6, 1, 2, 12};
    const std::array<CookedAnimationInterpolation, 4> modes = {
        CookedAnimationInterpolation::Linear, CookedAnimationInterpolation::Linear,
        CookedAnimationInterpolation::CubicSpline, CookedAnimationInterpolation::CubicSpline};
    CHECK(valueCounts.size() == 4);  // literal row count
    for (std::size_t row = 0; row < 4; ++row) {
        ClipBuilder clip;
        clip.add(1, CookedAnimationPath::Translation, modes[row], {0.0F, 1.0F},
                 std::vector<Vec4>(valueCounts[row], Vec4{}));
        const AnimationCookResult r = cook(clip.channels());
        CHECK((r.status == AnimationCookStatus::Invalid));
        CHECK(mentions(r.message, "values over"));
    }
    // The anti-vacuity twin: the two correct shapes are accepted.
    ClipBuilder linear;
    linear.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {0.0F, 1.0F},
               std::vector<Vec4>(2, Vec4{}));
    CHECK((cook(linear.channels()).status == AnimationCookStatus::Ok));
    ClipBuilder cubic;
    cubic.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::CubicSpline, {0.0F, 1.0F},
              std::vector<Vec4>(6, Vec4{}));
    CHECK((cook(cubic.channels()).status == AnimationCookStatus::Ok));
}

TEST_CASE("animation cook: each of the three caps refuses on its own (KA13)") {
    // The channel cap, checked before anything is walked.
    const std::vector<AnimationCookChannel> many(MAX_COOKED_ANIMATION_CHANNELS + 1);
    const AnimationCookResult rc = cook(std::span<const AnimationCookChannel>(many));
    CHECK((rc.status == AnimationCookStatus::Invalid));
    CHECK(mentions(rc.message, "over the cap"));
    CHECK(mentions(rc.message, "channels"));

    // The VALUE cap is checked before the KEY cap, and the order is load-bearing rather than
    // arbitrary: valueTotal is at most 3 x keyTotal, so a key total inside its cap can never carry a
    // value total outside its own. Checking keys first would make the value arm unreachable -- an
    // unwitnessable cap is one nothing can prove is wired up at all.
    {
        const std::size_t keys = MAX_COOKED_ANIMATION_KEYS + 1;
        std::vector<float> times(keys);
        for (std::size_t k = 0; k < keys; ++k) {
            times[k] = static_cast<float>(k);
        }
        std::vector<Vec4> values(keys * 3, Vec4{});
        AnimationCookChannel channel;
        channel.path = CookedAnimationPath::Scale;
        channel.interpolation = CookedAnimationInterpolation::CubicSpline;
        channel.times = std::span<const float>(times);
        channel.values = std::span<const Vec4>(values);
        const std::array<AnimationCookChannel, 1> one = {channel};
        const AnimationCookResult rv = cook(std::span<const AnimationCookChannel>(one));
        CHECK((rv.status == AnimationCookStatus::Invalid));
        CHECK(mentions(rv.message, "values, over the cap"));
        CHECK(mentions(rv.message, std::to_string(MAX_COOKED_ANIMATION_VALUES)));
    }
    // The key cap, reached by the same over-cap key total carrying ONE value per key.
    {
        const std::size_t keys = MAX_COOKED_ANIMATION_KEYS + 1;
        std::vector<float> times(keys);
        for (std::size_t k = 0; k < keys; ++k) {
            times[k] = static_cast<float>(k);
        }
        std::vector<Vec4> values(keys, Vec4{});
        AnimationCookChannel channel;
        channel.path = CookedAnimationPath::Scale;
        channel.interpolation = CookedAnimationInterpolation::Linear;
        channel.times = std::span<const float>(times);
        channel.values = std::span<const Vec4>(values);
        const std::array<AnimationCookChannel, 1> one = {channel};
        const AnimationCookResult rk = cook(std::span<const AnimationCookChannel>(one));
        CHECK((rk.status == AnimationCookStatus::Invalid));
        CHECK(mentions(rk.message, "keys, over the cap"));
        CHECK(mentions(rk.message, std::to_string(MAX_COOKED_ANIMATION_KEYS)));
    }
}

TEST_CASE("animation cook: w is forced to zero on Translation and Scale, verbatim on Rotation (KA14)") {
    ClipBuilder clip;
    clip.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(0.0F),
             {Vec4{1.0F, 2.0F, 3.0F, 42.0F}});
    clip.add(2, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, oneTime(0.0F),
             {Vec4{0.0F, 0.0F, 0.5F, 0.75F}});
    clip.add(3, CookedAnimationPath::Scale, CookedAnimationInterpolation::Linear, oneTime(0.0F),
             {Vec4{4.0F, 5.0F, 6.0F, -13.0F}});
    const AnimationCookResult r = cook(clip.channels());
    REQUIRE((r.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult p = parse(r.bytes);
    REQUIRE((p.status == CookedAnimationStatus::Ok));
    REQUIRE(p.animation.channels.size() == 3);

    // Emitted order is by node: 1 Translation, 2 Rotation, 3 Scale.
    const Vec4 translation = animationKeyValue(channelValueBytes(p.animation, 0), 0);
    CHECK(translation.x == doctest::Approx(1.0F));
    CHECK(translation.z == doctest::Approx(3.0F));
    CHECK(bits(translation.w) == bits(0.0F));

    const Vec4 rotation = animationKeyValue(channelValueBytes(p.animation, 1), 0);
    CHECK(rotation.z == doctest::Approx(0.5F));
    CHECK(rotation.w == doctest::Approx(0.75F));

    const Vec4 scale = animationKeyValue(channelValueBytes(p.animation, 2), 0);
    CHECK(scale.x == doctest::Approx(4.0F));
    CHECK(bits(scale.w) == bits(0.0F));
}

TEST_CASE("animation cook: the duration fold is a max over last keys, in EMISSION order (KA15)") {
    // The max over channels, and NOT the last channel's own last key.
    ClipBuilder several;
    several.add(9, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {0.0F, 4.0F},
                {Vec4{}, Vec4{}});
    several.add(1, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, {0.0F, 1.0F}, {Vec4{}, Vec4{}});
    const AnimationCookResult rs = cook(several.channels());
    REQUIRE((rs.status == AnimationCookStatus::Ok));
    CHECK(parse(rs.bytes).animation.durationSeconds == doctest::Approx(4.0F));

    // Only-negative times fold to 0, because the accumulator starts at 0 rather than -inf.
    ClipBuilder negative;
    negative.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {-3.0F, -1.0F},
                 {Vec4{}, Vec4{}});
    const AnimationCookResult rn = cook(negative.channels());
    REQUIRE((rn.status == AnimationCookStatus::Ok));
    CHECK(bits(parse(rn.bytes).animation.durationSeconds) == bits(0.0F));

    // A single-key clip's duration is that key's own time.
    ClipBuilder single;
    single.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(1.5F),
               oneValue(0.0F));
    const AnimationCookResult r1 = cook(single.channels());
    REQUIRE((r1.status == AnimationCookStatus::Ok));
    CHECK(parse(r1.bytes).animation.durationSeconds == doctest::Approx(1.5F));

    // The EMISSION-order property: the same channels supplied in the other order produce byte-
    // identical output, header included. An input-order fold would be invisible in the value and
    // visible in the bytes only for a signed zero -- so the assertion is on the whole buffer.
    ClipBuilder reversed;
    reversed.add(1, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, {0.0F, 1.0F},
                 {Vec4{}, Vec4{}});
    reversed.add(9, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {0.0F, 4.0F},
                 {Vec4{}, Vec4{}});
    const AnimationCookResult rr = cook(reversed.channels());
    REQUIRE((rr.status == AnimationCookStatus::Ok));
    CHECK(rr.bytes == rs.bytes);
}

TEST_CASE("animation cook: firstKey and firstValue are running sums over differing multipliers (KA16)") {
    ClipBuilder clip;
    clip.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {0.0F, 1.0F, 2.0F},
             std::vector<Vec4>(3, Vec4{}));
    clip.add(2, CookedAnimationPath::Rotation, CookedAnimationInterpolation::CubicSpline, {0.0F, 1.0F},
             std::vector<Vec4>(6, Vec4{}));
    clip.add(3, CookedAnimationPath::Scale, CookedAnimationInterpolation::Step, {0.0F, 1.0F, 2.0F, 3.0F},
             std::vector<Vec4>(4, Vec4{}));
    const AnimationCookResult r = cook(clip.channels());
    REQUIRE((r.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult p = parse(r.bytes);
    REQUIRE((p.status == CookedAnimationStatus::Ok));
    REQUIRE(p.animation.channels.size() == 3);

    CHECK(p.animation.keyCount == 9);
    CHECK(p.animation.valueCount == 13);
    CHECK(p.animation.channels[0].firstKey == 0);
    CHECK(p.animation.channels[0].firstValue == 0);
    CHECK(p.animation.channels[1].firstKey == 3);
    CHECK(p.animation.channels[1].firstValue == 3);
    CHECK(p.animation.channels[2].firstKey == 5);
    CHECK(p.animation.channels[2].firstValue == 9);
    CHECK(channelTimeBytes(p.animation, 2).size() == 16);
    CHECK(channelValueBytes(p.animation, 1).size() == 96);
}

TEST_CASE("animation cook: the one padding site is present, absent and zero-filled (KA17)") {
    // keyCount % 4 == 3 -> a four-byte site at [124, 128).
    ClipBuilder present;
    present.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {0.0F, 1.0F, 2.0F},
                std::vector<Vec4>(3, Vec4{}));
    const AnimationCookResult rp = cook(present.channels());
    REQUIRE((rp.status == AnimationCookStatus::Ok));
    CHECK(rp.bytes.size() == 176);
    const CookedAnimationParseResult pp = parse(rp.bytes);
    REQUIRE((pp.status == CookedAnimationStatus::Ok));
    CHECK(pp.animation.timesDataOffset == 112);
    CHECK(pp.animation.valuesDataOffset == 128);
    CHECK(cookedAnimationTimesPadding(3) == 4);
    for (std::size_t at = 124; at < 128; ++at) {
        CHECK(rp.bytes[at] == std::byte{0});
    }

    // keyCount % 4 == 0 -> no site at all; the values region begins the byte after the times region.
    ClipBuilder absent;
    absent.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {0.0F, 1.0F, 2.0F, 3.0F},
               std::vector<Vec4>(4, Vec4{}));
    const AnimationCookResult ra = cook(absent.channels());
    REQUIRE((ra.status == AnimationCookStatus::Ok));
    CHECK(ra.bytes.size() == 192);
    const CookedAnimationParseResult pa = parse(ra.bytes);
    REQUIRE((pa.status == CookedAnimationStatus::Ok));
    CHECK(pa.animation.timesDataOffset == 112);
    CHECK(pa.animation.valuesDataOffset == 128);
    CHECK(cookedAnimationTimesPadding(4) == 0);
}

TEST_CASE("animation cook: the warning list is capped and the total is not (KA18)") {
    ClipBuilder clip;
    // Five channels that survive, twenty-five that drop -- thirty in all.
    for (std::uint32_t i = 0; i < 5; ++i) {
        clip.add(1000 + i, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(0.0F),
                 oneValue(0.0F));
    }
    for (std::uint32_t i = 0; i < 25; ++i) {
        clip.add(2000 + i, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, {}, {});
    }
    const AnimationCookResult r = cook(clip.channels());
    CHECK((r.status == AnimationCookStatus::Truncated));
    CHECK(r.warnings.size() == MAX_COOK_WARNINGS);
    CHECK(r.warnings.size() == 20);  // the cap, as a literal
    CHECK(r.warningTotal == 25);
    CHECK(mentions(r.message, "25 of 30"));
    CHECK((parse(r.bytes).status == CookedAnimationStatus::Ok));
}

TEST_CASE("animation cook: cook -> parse round trips every field, in canonical order (KA19)") {
    const MinimalClipData minimalData;
    const std::array<AnimationCookChannel, 1> minimal = minimalChannels(minimalData);
    const AnimationCookResult rm = cook(std::span<const AnimationCookChannel>(minimal));
    REQUIRE((rm.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult pm = parse(rm.bytes);
    REQUIRE((pm.status == CookedAnimationStatus::Ok));
    CHECK(pm.animation.formatVersion == 1);
    CHECK(!pm.animation.sourceGuid.valid());
    CHECK(pm.animation.sourceAnimationIndex == 0);
    CHECK(pm.animation.keyCount == 2);
    CHECK(pm.animation.valueCount == 2);
    CHECK(pm.animation.durationSeconds == doctest::Approx(0.5F));
    REQUIRE(pm.animation.channels.size() == 1);
    CHECK(pm.animation.channels[0].targetNodeLocalId == 5);
    CHECK((pm.animation.channels[0].path == CookedAnimationPath::Rotation));
    CHECK((pm.animation.channels[0].interpolation == CookedAnimationInterpolation::Linear));
    CHECK(animationKeyTime(channelTimeBytes(pm.animation, 0), 1) == doctest::Approx(0.5F));
    CHECK(animationKeyValue(channelValueBytes(pm.animation, 0), 1).z == doctest::Approx(1.0F));

    const MixedClipData mixedData;
    const std::array<AnimationCookChannel, 3> mixed = mixedChannels(mixedData);
    const AnimationCookResult rx =
        cook(std::span<const AnimationCookChannel>(mixed), MIXED_GUID, MIXED_ANIMATION_INDEX);
    REQUIRE((rx.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult px = parse(rx.bytes);
    REQUIRE((px.status == CookedAnimationStatus::Ok));
    CHECK(px.animation.sourceGuid.hi == 0x0123456789ABCDEFULL);
    CHECK(px.animation.sourceGuid.lo == 0xFEDCBA9876543210ULL);
    CHECK(px.animation.sourceAnimationIndex == 2);
    CHECK(px.animation.keyCount == 7);
    CHECK(px.animation.valueCount == 11);
    CHECK(px.animation.durationSeconds == doctest::Approx(2.0F));
    REQUIRE(px.animation.channels.size() == 3);
    CHECK(px.animation.channels[0].targetNodeLocalId == 3);
    CHECK((px.animation.channels[0].path == CookedAnimationPath::Rotation));
    CHECK(px.animation.channels[1].targetNodeLocalId == 7);
    CHECK((px.animation.channels[1].path == CookedAnimationPath::Translation));
    CHECK(px.animation.channels[2].targetNodeLocalId == 7);
    CHECK((px.animation.channels[2].path == CookedAnimationPath::Scale));
    CHECK(px.animation.channels[0].firstKey == 0);
    CHECK(px.animation.channels[1].firstKey == 3);
    CHECK(px.animation.channels[2].firstKey == 5);
    CHECK(px.animation.channels[0].firstValue == 0);
    CHECK(px.animation.channels[1].firstValue == 3);
    CHECK(px.animation.channels[2].firstValue == 5);

    // A twelve-channel property case: four nodes x three paths, cooked in three different input
    // orders, all producing identical bytes and identical parsed records.
    const std::array<std::size_t, 3> rotations = {0, 5, 7};
    CHECK(rotations.size() == 3);  // literal row count
    std::vector<AnimationCookResult> cooked;
    std::deque<ClipBuilder> builders;
    for (const std::size_t shift : rotations) {
        builders.emplace_back();
        ClipBuilder& clip = builders.back();
        for (std::size_t n = 0; n < 12; ++n) {
            const std::size_t slot = (n + shift) % 12;
            const auto node = static_cast<std::uint32_t>(slot / 3);
            const auto path = static_cast<CookedAnimationPath>(slot % 3);
            clip.add(node, path, CookedAnimationInterpolation::Linear, {0.0F, static_cast<float>(slot) + 1.0F},
                     std::vector<Vec4>(2, Vec4{static_cast<float>(slot), 0.0F, 0.0F, 0.0F}));
        }
        cooked.push_back(cook(clip.channels()));
        REQUIRE((cooked.back().status == AnimationCookStatus::Ok));
    }
    REQUIRE(cooked.size() == 3);
    CHECK(cooked[0].bytes == cooked[1].bytes);
    CHECK(cooked[0].bytes == cooked[2].bytes);
    const CookedAnimationParseResult p12 = parse(cooked[0].bytes);
    REQUIRE((p12.status == CookedAnimationStatus::Ok));
    REQUIRE(p12.animation.channels.size() == 12);
    for (std::uint32_t i = 0; i < 12; ++i) {
        CHECK(p12.animation.channels[i].targetNodeLocalId == i / 3);
        CHECK((p12.animation.channels[i].path == static_cast<CookedAnimationPath>(i % 3)));
    }
}

TEST_CASE("animation cook: bits travel through the cook unchanged, values do not (KA20)") {
    constexpr std::uint32_t SIGNALLING = 0x7FA00000U;
    const auto snan = std::bit_cast<float>(SIGNALLING);
    ClipBuilder clip;
    clip.add(1, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, {-0.0F},
             {Vec4{snan, -0.0F, snan, -0.0F}});
    const AnimationCookResult r = cook(clip.channels());
    REQUIRE((r.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult p = parse(r.bytes);
    REQUIRE((p.status == CookedAnimationStatus::Ok));

    CHECK(bits(animationKeyTime(channelTimeBytes(p.animation, 0), 0)) == bits(-0.0F));
    const Vec4 value = animationKeyValue(channelValueBytes(p.animation, 0), 0);
    CHECK(bits(value.x) == SIGNALLING);
    CHECK(bits(value.y) == bits(-0.0F));
    CHECK(bits(value.z) == SIGNALLING);
    CHECK(bits(value.w) == bits(-0.0F));
    // The DURATION is folded from 0.0f, and std::max(0.0f, -0.0f) returns the accumulator -- so the
    // header carries a positive zero while the times region carries the negative one it was given.
    CHECK(bits(p.animation.durationSeconds) == bits(0.0F));
}

TEST_CASE("animation cook: message is empty iff Ok and bytes are empty iff Invalid (KA21)") {
    ClipBuilder ok;
    ok.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(0.0F), oneValue(0.0F));
    ClipBuilder truncated;
    truncated.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(0.0F),
                  oneValue(0.0F));
    truncated.add(2, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear, {}, {});
    ClipBuilder allDropped;
    allDropped.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, {}, {});
    ClipBuilder duplicate;
    duplicate.add(1, CookedAnimationPath::Scale, CookedAnimationInterpolation::Linear, oneTime(0.0F), oneValue(0.0F));
    duplicate.add(1, CookedAnimationPath::Scale, CookedAnimationInterpolation::Linear, oneTime(1.0F), oneValue(0.0F));
    ClipBuilder unsorted;
    unsorted.add(1, CookedAnimationPath::Scale, CookedAnimationInterpolation::Linear, {1.0F, 0.0F}, {Vec4{}, Vec4{}});
    ClipBuilder badCode;
    badCode.addRawCodes(1, 9, 0, oneTime(0.0F), oneValue(0.0F));

    std::vector<AnimationCookResult> arms;
    arms.push_back(cook(ok.channels()));
    arms.push_back(cook(truncated.channels()));
    arms.push_back(cook(allDropped.channels()));
    arms.push_back(cook(duplicate.channels()));
    arms.push_back(cook(unsorted.channels()));
    arms.push_back(cook(badCode.channels()));
    arms.push_back(cook(std::span<const AnimationCookChannel>{}));
    CHECK(arms.size() == 7);  // literal arm count
    for (const AnimationCookResult& r : arms) {
        CHECK(r.message.empty() == (r.status == AnimationCookStatus::Ok));
        CHECK(r.bytes.empty() == (r.status == AnimationCookStatus::Invalid));
    }
}

TEST_CASE("animation cook: the GUID and the source animation index are written and read back (KA22)") {
    ClipBuilder clip;
    clip.add(1, CookedAnimationPath::Translation, CookedAnimationInterpolation::Linear, oneTime(0.0F), oneValue(0.0F));

    const AnimationCookResult nil = cook(clip.channels(), Guid{}, 0);
    REQUIRE((nil.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult pn = parse(nil.bytes);
    REQUIRE((pn.status == CookedAnimationStatus::Ok));
    CHECK(!pn.animation.sourceGuid.valid());
    CHECK(pn.animation.sourceAnimationIndex == 0);

    const AnimationCookResult real = cook(clip.channels(), MIXED_GUID, 17);
    REQUIRE((real.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult pr = parse(real.bytes);
    REQUIRE((pr.status == CookedAnimationStatus::Ok));
    CHECK(pr.animation.sourceGuid.hi == 0x0123456789ABCDEFULL);
    CHECK(pr.animation.sourceGuid.lo == 0xFEDCBA9876543210ULL);
    CHECK(pr.animation.sourceAnimationIndex == 17);

    // Deterministic: the same input twice is the same bytes, and the two GUIDs differ only where
    // they should.
    CHECK(cook(clip.channels(), MIXED_GUID, 17).bytes == real.bytes);
    CHECK(nil.bytes.size() == real.bytes.size());
    CHECK(nil.bytes != real.bytes);
}
