// tests/editor/animation_cook_source_test.cpp -- task 3.5.2: the ImportedModel -> animation cook
// adapter. A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the skeleton_cook_source_test.cpp precedent): animation_cook_source.hpp depends
// only on aero/editor/model_import.hpp and aero/assets/animation_cook.hpp, and aero::assets is a
// PUBLIC, UNGATED dependency of aero_editor_core -- so every case here must be PRESENT and PASSING
// in all three build configurations. No GPU, no window, no ImGui context, no sleeps.
//
// Most cases drive a HAND-BUILT ImportedModel rather than a document, deliberately: the trap this
// adapter exists to discharge -- a targetNode localId that is NOT a position, written through
// unconverted -- is invisible in glTF, where localIds and positions always coincide. AS1-AS3 are the
// real-import closure that keeps the hand-built models honest, and AS9 is the one no document can
// express.
#include <aero/assets/animation_cook.hpp>
#include <aero/assets/cooked_animation.hpp>
#include <aero/editor/animation_cook_source.hpp>
#include <aero/editor/model_import.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies std::string_view operands
// through operator<<(std::ostream&, std::string_view), which MS STL defines inline in <string_view>
// against an INCOMPLETE std::basic_ostream. Written when the TU was created.
#include <ostream>
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
using engine::assets::parseCookedAnimation;
using engine::editor::animationCookChannels;
using engine::editor::animationCookChannelSpans;
using engine::editor::AnimationInterpolation;
using engine::editor::AnimationPath;
using engine::editor::AnimationSourceResult;
using engine::editor::cookImportedAnimation;
using engine::editor::ImportDepth;
using engine::editor::ImportedAnimation;
using engine::editor::ImportedAnimationChannel;
using engine::editor::ImportedModel;
using engine::editor::ImportedNode;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::INVALID_SUBASSET;
using engine::editor::MAX_IMPORT_WARNINGS;

// ---- AS4, the compile-time half. The two enumerations live in layers that may never include each
// other, so THIS TU is the one place both are visible and the correspondence can be pinned at all.
// Six assertions, one per enumerator pair, on the UNDERLYING values -- the CookedVertexSemantic
// precedent. A renumbering on either side is a compile error here rather than a silently mis-coded
// artifact. (AC-28.)
static_assert(static_cast<std::uint32_t>(AnimationPath::Translation) ==
              static_cast<std::uint32_t>(CookedAnimationPath::Translation));
static_assert(static_cast<std::uint32_t>(AnimationPath::Rotation) ==
              static_cast<std::uint32_t>(CookedAnimationPath::Rotation));
static_assert(static_cast<std::uint32_t>(AnimationPath::Scale) ==
              static_cast<std::uint32_t>(CookedAnimationPath::Scale));
static_assert(static_cast<std::uint32_t>(AnimationInterpolation::Linear) ==
              static_cast<std::uint32_t>(CookedAnimationInterpolation::Linear));
static_assert(static_cast<std::uint32_t>(AnimationInterpolation::Step) ==
              static_cast<std::uint32_t>(CookedAnimationInterpolation::Step));
static_assert(static_cast<std::uint32_t>(AnimationInterpolation::CubicSpline) ==
              static_cast<std::uint32_t>(CookedAnimationInterpolation::CubicSpline));

namespace {

[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

[[nodiscard]] std::string readFixture(const std::string& path) {
    const scene_golden::FileBytes bytes = scene_golden::readBytes(path);
    REQUIRE_MESSAGE(bytes.ok, bytes.error);
    return bytes.text;
}

[[nodiscard]] bool sameBits(float a, float b) noexcept {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

[[nodiscard]] bool anyWarningContains(const std::vector<std::string>& warnings, std::string_view needle) {
    for (const std::string& warning : warnings) {
        if (warning.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// One channel with keys, on a node named by localId. `times` and `values` are supplied whole so a
// caller can build a CUBICSPLINE channel (three values per key) without a second helper.
[[nodiscard]] ImportedAnimationChannel channel(std::uint32_t targetNode, AnimationPath path,
                                               AnimationInterpolation interpolation, std::vector<float> times,
                                               std::vector<Vec4> values) {
    ImportedAnimationChannel out;
    out.targetNode = targetNode;
    out.path = path;
    out.interpolation = interpolation;
    out.times = std::move(times);
    out.values = std::move(values);
    return out;
}

// A LINEAR translation channel with two keys, the shape most cases here only need to exist.
[[nodiscard]] ImportedAnimationChannel simpleChannel(std::uint32_t targetNode) {
    return channel(targetNode, AnimationPath::Translation, AnimationInterpolation::Linear, {0.0F, 1.0F},
                   {Vec4{1.0F, 0.0F, 0.0F, 0.0F}, Vec4{2.0F, 0.0F, 0.0F, 0.0F}});
}

[[nodiscard]] ImportedNode node(std::uint32_t localId, std::uint32_t parent) {
    ImportedNode n;
    n.name = "node" + std::to_string(localId);
    n.localId = localId;
    n.parent = parent;
    return n;
}

// The FBX shape: four nodes in a chain whose localIds are raw ids, non-dense and non-positional.
// nodes[localId] is out of bounds for every one of them, which is what makes this model the case
// that reddens if anyone resolves targetNode through a position table.
[[nodiscard]] ImportedModel fbxShapedModel() {
    ImportedModel model;
    model.nodes.push_back(node(100, INVALID_SUBASSET));
    model.nodes.push_back(node(205, 100));
    model.nodes.push_back(node(300, 205));
    model.nodes.push_back(node(407, 300));
    model.roots.push_back(100);

    ImportedAnimation clip;
    clip.name = "Chain";
    clip.duration = 1.0F;
    clip.channels.push_back(simpleChannel(100));
    clip.channels.push_back(simpleChannel(205));
    clip.channels.push_back(simpleChannel(300));
    clip.channels.push_back(simpleChannel(407));
    model.animations.push_back(std::move(clip));
    return model;
}

// Adapter -> spans -> cook, the three halves composed by hand. Returned by value so a caller can
// keep the bytes alive while it parses them.
[[nodiscard]] AnimationCookResult cookByHand(const AnimationSourceResult& source, Guid guid, std::uint32_t clipIndex) {
    std::vector<AnimationCookChannel> spans;
    animationCookChannelSpans(source, spans);
    AnimationCookInput input;
    input.sourceGuid = guid;
    input.sourceAnimationIndex = clipIndex;
    input.channels = spans;
    return cookAnimation(input);
}

// The real-import closure AS1-AS3 share: import skinned.gltf at Full depth, cook the named clip
// through the convenience, parse the bytes back and compare EVERY field against the import itself.
// The fixture's three clips are one per interpolation mode, on three different nodes.
void checkClipRoundTrip(std::uint32_t clipIndex, CookedAnimationPath expectedPath,
                        CookedAnimationInterpolation expectedInterpolation) {
    const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/skinned.gltf");
    const ImportResult imported =
        importModel("skinned.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE((imported.status == ImportStatus::Ok));
    REQUIRE(imported.model.animations.size() == 3);
    const ImportedAnimation& clip = imported.model.animations[clipIndex];
    // EXACTLY ONE surviving channel: clip 0 declares a `weights` channel beside its translation one
    // and the importer drops it (there is no Weights enumerator, by design), so a second channel
    // here would mean the importer had started inventing one.
    REQUIRE(clip.channels.size() == 1);
    const ImportedAnimationChannel& sourceChannel = clip.channels[0];

    const AnimationCookResult cooked = cookImportedAnimation(imported.model, clipIndex, Guid{});
    REQUIRE((cooked.status == AnimationCookStatus::Ok));
    CHECK(cooked.message.empty());

    const CookedAnimationParseResult parsed = parseCookedAnimation(std::span<const std::byte>(cooked.bytes));
    REQUIRE((parsed.status == CookedAnimationStatus::Ok));
    CHECK(parsed.animation.sourceAnimationIndex == clipIndex);
    CHECK(sameBits(parsed.animation.durationSeconds, clip.duration));
    REQUIRE(parsed.animation.channels.size() == 1);

    const auto& emitted = parsed.animation.channels[0];
    // The write-through, on a real document. glTF cannot separate a localId from a position, which
    // is exactly why AS9 exists -- but this half still pins that no OTHER transformation happens.
    CHECK(emitted.targetNodeLocalId == sourceChannel.targetNode);
    CHECK((emitted.path == expectedPath));
    CHECK((emitted.interpolation == expectedInterpolation));
    CHECK(emitted.keyCount == static_cast<std::uint32_t>(sourceChannel.times.size()));
    CHECK(emitted.valueCount == static_cast<std::uint32_t>(sourceChannel.values.size()));

    const std::span<const std::byte> times = channelTimeBytes(parsed.animation, 0);
    for (std::uint32_t k = 0; k < emitted.keyCount; ++k) {
        CHECK(sameBits(animationKeyTime(times, k), sourceChannel.times[k]));
    }
    const std::span<const std::byte> values = channelValueBytes(parsed.animation, 0);
    const bool carriesW = expectedPath == CookedAnimationPath::Rotation;
    for (std::uint32_t v = 0; v < emitted.valueCount; ++v) {
        const Vec4 read = animationKeyValue(values, v);
        const Vec4& want = sourceChannel.values[v];
        CHECK(sameBits(read.x, want.x));
        CHECK(sameBits(read.y, want.y));
        CHECK(sameBits(read.z, want.z));
        CHECK(sameBits(read.w, carriesW ? want.w : 0.0F));
    }
}

}  // namespace

TEST_CASE("animation_cook_source: a real STEP translation clip round-trips field for field (AS1)") {
    checkClipRoundTrip(0, CookedAnimationPath::Translation, CookedAnimationInterpolation::Step);
}

TEST_CASE("animation_cook_source: a real LINEAR rotation clip keeps its w component (AS2)") {
    checkClipRoundTrip(1, CookedAnimationPath::Rotation, CookedAnimationInterpolation::Linear);
}

TEST_CASE("animation_cook_source: a real CUBICSPLINE scale clip carries three values per key (AS3)") {
    checkClipRoundTrip(2, CookedAnimationPath::Scale, CookedAnimationInterpolation::CubicSpline);

    // The cubic multiplier, asserted at this tier too: the importer's own INV-M6 says the source
    // holds 3x, and the artifact must not quietly collapse it.
    const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/skinned.gltf");
    const ImportResult imported =
        importModel("skinned.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE((imported.status == ImportStatus::Ok));
    const AnimationCookResult cooked = cookImportedAnimation(imported.model, 2, Guid{});
    REQUIRE((cooked.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult parsed = parseCookedAnimation(std::span<const std::byte>(cooked.bytes));
    REQUIRE((parsed.status == CookedAnimationStatus::Ok));
    REQUIRE(parsed.animation.channels.size() == 1);
    CHECK(parsed.animation.channels[0].valueCount == 3 * parsed.animation.channels[0].keyCount);
}

TEST_CASE("animation_cook_source: the two enum mirrors agree value by value (AS4)") {
    // The compile-time half is the six static_asserts at the top of this TU. This is the runtime
    // half: the adapter's own mapping, driven through the PUBLIC surface, value by value -- so a
    // switch that mapped Rotation to Scale would redden here even if the underlying values matched.
    struct PathCase {
        AnimationPath source;
        CookedAnimationPath expected;
    };
    const std::array<PathCase, 3> paths = {{
        {AnimationPath::Translation, CookedAnimationPath::Translation},
        {AnimationPath::Rotation, CookedAnimationPath::Rotation},
        {AnimationPath::Scale, CookedAnimationPath::Scale},
    }};
    CHECK(paths.size() == 3);  // literal row count

    struct ModeCase {
        AnimationInterpolation source;
        CookedAnimationInterpolation expected;
        std::size_t valuesPerKey;
    };
    const std::array<ModeCase, 3> modes = {{
        {AnimationInterpolation::Linear, CookedAnimationInterpolation::Linear, 1},
        {AnimationInterpolation::Step, CookedAnimationInterpolation::Step, 1},
        {AnimationInterpolation::CubicSpline, CookedAnimationInterpolation::CubicSpline, 3},
    }};
    CHECK(modes.size() == 3);  // literal row count

    for (const PathCase& pathCase : paths) {
        for (const ModeCase& modeCase : modes) {
            std::vector<Vec4> values;
            for (std::size_t v = 0; v < 2 * modeCase.valuesPerKey; ++v) {
                values.push_back(Vec4{static_cast<float>(v), 0.5F, 0.25F, 0.125F});
            }
            ImportedModel model;
            model.nodes.push_back(node(7, INVALID_SUBASSET));
            model.roots.push_back(7);
            ImportedAnimation clip;
            clip.channels.push_back(channel(7, pathCase.source, modeCase.source, {0.0F, 1.0F}, std::move(values)));
            model.animations.push_back(std::move(clip));

            const AnimationSourceResult source = animationCookChannels(model, 0);
            REQUIRE(source.ok);
            REQUIRE(source.channels.size() == 1);
            CHECK((source.channels[0].path == pathCase.expected));
            CHECK((source.channels[0].interpolation == modeCase.expected));
        }
    }
}

TEST_CASE("animation_cook_source: an out-of-range clip index names the real count (AS5)") {
    const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/skinned.gltf");
    const ImportResult imported =
        importModel("skinned.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE((imported.status == ImportStatus::Ok));
    REQUIRE(imported.model.animations.size() == 3);

    const AnimationSourceResult source = animationCookChannels(imported.model, 3);
    CHECK_FALSE(source.ok);
    CHECK(source.channels.empty());
    // THE COUNT COMES FROM THE MODEL. A message printing a hard-coded number would satisfy AS6 alone,
    // which is why the same refusal is asserted at two different counts.
    CHECK(source.error.find("has 3 animation") != std::string::npos);
    CHECK(source.error.find("clip index 3") != std::string::npos);
}

TEST_CASE("animation_cook_source: a model with no animations refuses naming zero (AS6)") {
    ImportedModel model;
    model.nodes.push_back(node(0, INVALID_SUBASSET));
    model.roots.push_back(0);

    const AnimationSourceResult source = animationCookChannels(model, 0);
    CHECK_FALSE(source.ok);
    CHECK(source.error.find("has 0 animation") != std::string::npos);
    CHECK(source.warnings.empty());

    // And through the convenience: an adapter error is an Invalid cook with no bytes.
    const AnimationCookResult cooked = cookImportedAnimation(model, 0, Guid{});
    CHECK((cooked.status == AnimationCookStatus::Invalid));
    CHECK(cooked.bytes.empty());
    CHECK(cooked.message.find("has 0 animation") != std::string::npos);
}

TEST_CASE("animation_cook_source: a Structure-depth model is refused by name, in both shapes (AS7)") {
    SUBCASE("the glTF shape: channels recorded with empty times") {
        ImportedModel model;
        model.nodes.push_back(node(0, INVALID_SUBASSET));
        model.roots.push_back(0);
        ImportedAnimation clip;
        clip.channels.push_back(channel(0, AnimationPath::Translation, AnimationInterpolation::Linear, {}, {}));
        clip.channels.push_back(channel(0, AnimationPath::Rotation, AnimationInterpolation::Linear, {}, {}));
        model.animations.push_back(std::move(clip));

        const AnimationSourceResult source = animationCookChannels(model, 0);
        CHECK_FALSE(source.ok);
        CHECK(source.error.find("Structure depth") != std::string::npos);
    }
    SUBCASE("the FBX shape: a clip shell with no channels at all") {
        ImportedModel model;
        model.nodes.push_back(node(0, INVALID_SUBASSET));
        model.roots.push_back(0);
        model.animations.emplace_back();

        const AnimationSourceResult source = animationCookChannels(model, 0);
        CHECK_FALSE(source.ok);
        CHECK(source.error.find("Structure depth") != std::string::npos);
    }
    SUBCASE("the real thing: skinned.gltf imported at Structure depth") {
        const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/skinned.gltf");
        const ImportResult imported =
            importModel("skinned.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
        REQUIRE((imported.status == ImportStatus::Ok));
        REQUIRE(imported.model.animations.size() == 3);
        for (std::uint32_t clipIndex = 0; clipIndex < 3; ++clipIndex) {
            const AnimationSourceResult source = animationCookChannels(imported.model, clipIndex);
            CHECK_FALSE(source.ok);
            CHECK(source.error.find("Structure depth") != std::string::npos);
        }
    }
}

TEST_CASE("animation_cook_source: the multi-clip advisory rides every successful clip cook (AS8)") {
    const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/skinned.gltf");
    const ImportResult imported =
        importModel("skinned.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE((imported.status == ImportStatus::Ok));
    REQUIRE(imported.model.animations.size() == 3);

    for (std::uint32_t clipIndex = 0; clipIndex < 3; ++clipIndex) {
        const AnimationSourceResult source = animationCookChannels(imported.model, clipIndex);
        REQUIRE(source.ok);
        CHECK(anyWarningContains(source.warnings, "has 3 animations"));
        CHECK(anyWarningContains(source.warnings, "cooking clip " + std::to_string(clipIndex)));
        // And it reaches the CLI's own printer through the convenience, with no second implementation.
        const AnimationCookResult cooked = cookImportedAnimation(imported.model, clipIndex, Guid{});
        REQUIRE((cooked.status == AnimationCookStatus::Ok));
        CHECK(anyWarningContains(cooked.warnings, "has 3 animations"));
        CHECK(cooked.warningTotal >= 1);
    }

    // A single-clip model says nothing at all -- the advisory is about a CHOICE, and there is none.
    const ImportedModel single = fbxShapedModel();
    REQUIRE(single.animations.size() == 1);
    const AnimationSourceResult quiet = animationCookChannels(single, 0);
    REQUIRE(quiet.ok);
    CHECK(quiet.warnings.empty());
}

TEST_CASE("animation_cook_source: targetNode is written through UNCONVERTED (AS9)") {
    // THE case for AC-25, and the one no document can express: this model's node localIds are
    // 100/205/300/407, so nodes[targetNode] is out of bounds for every one of them and a
    // localId -> position map would answer 0/1/2/3. glTF cannot separate the two, which is why this
    // model is hand-built. If anyone "fixes" this adapter to resolve through a position table --
    // the direction all four previous consumers of the localId rule go -- this case reddens on the
    // very first channel.
    const ImportedModel model = fbxShapedModel();
    const AnimationSourceResult source = animationCookChannels(model, 0);
    REQUIRE(source.ok);
    REQUIRE(source.channels.size() == 4);

    const std::array<std::uint32_t, 4> expected = {100, 205, 300, 407};
    CHECK(expected.size() == 4);  // literal row count
    for (std::size_t c = 0; c < 4; ++c) {
        CHECK(source.channels[c].targetLocalId == expected[c]);
        CHECK(source.channels[c].targetLocalId == model.animations[0].channels[c].targetNode);
    }

    // And through the whole chain, so the property is asserted on the ARTIFACT rather than only on
    // the adapter's intermediate. The cook sorts ascending by (node, path) and 100 < 205 < 300 < 407
    // already, so the emitted order is the input order here.
    const AnimationCookResult cooked = cookImportedAnimation(model, 0, Guid{});
    REQUIRE((cooked.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult parsed = parseCookedAnimation(std::span<const std::byte>(cooked.bytes));
    REQUIRE((parsed.status == CookedAnimationStatus::Ok));
    REQUIRE(parsed.animation.channels.size() == 4);
    for (std::size_t c = 0; c < 4; ++c) {
        CHECK(parsed.animation.channels[c].targetNodeLocalId == expected[c]);
    }
}

TEST_CASE("animation_cook_source: two channels on one (node, path) pair compose into a refusal (AS10)") {
    // The adapter PASSES THEM THROUGH and the cook refuses. Pinned so the split is a decision rather
    // than an accident: refusing in one place keeps the two layers' stories identical, and the
    // message that reaches a user names the node and the path.
    ImportedModel model;
    model.nodes.push_back(node(42, INVALID_SUBASSET));
    model.roots.push_back(42);
    ImportedAnimation clip;
    clip.channels.push_back(simpleChannel(42));
    clip.channels.push_back(simpleChannel(42));
    model.animations.push_back(std::move(clip));

    const AnimationSourceResult source = animationCookChannels(model, 0);
    REQUIRE(source.ok);
    CHECK(source.channels.size() == 2);  // the adapter drops nothing and sorts nothing

    const AnimationCookResult cooked = cookImportedAnimation(model, 0, Guid{});
    CHECK((cooked.status == AnimationCookStatus::Invalid));
    CHECK(cooked.bytes.empty());
    CHECK(cooked.message.find("node 42") != std::string::npos);
    CHECK(cooked.message.find("Translation") != std::string::npos);
}

TEST_CASE("animation_cook_source: a channel targeting the invalid sentinel is dropped with a warning (AS11)") {
    ImportedModel model;
    model.nodes.push_back(node(3, INVALID_SUBASSET));
    model.roots.push_back(3);
    ImportedAnimation clip;
    clip.channels.push_back(simpleChannel(INVALID_SUBASSET));
    clip.channels.push_back(simpleChannel(3));
    model.animations.push_back(std::move(clip));

    const AnimationSourceResult source = animationCookChannels(model, 0);
    REQUIRE(source.ok);
    REQUIRE(source.channels.size() == 1);
    // The survivor is the SECOND channel, so the drop cannot be mistaken for a truncation.
    CHECK(source.channels[0].targetLocalId == 3U);
    CHECK(anyWarningContains(source.warnings, "channel 0"));
    CHECK(anyWarningContains(source.warnings, "no target node"));
    // The sentinel is never written through as node 4294967295, which would bind to nothing and look
    // exactly like a clip that simply does not animate that joint.
    const AnimationCookResult cooked = cookImportedAnimation(model, 0, Guid{});
    REQUIRE((cooked.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult parsed = parseCookedAnimation(std::span<const std::byte>(cooked.bytes));
    REQUIRE((parsed.status == CookedAnimationStatus::Ok));
    REQUIRE(parsed.animation.channels.size() == 1);
    CHECK(parsed.animation.channels[0].targetNodeLocalId == 3U);
}

TEST_CASE("animation_cook_source: the result survives a copy and the original's death (AS12)") {
    // THE OWNERSHIP CASE (AC-23, R7). It is impossible to write against the rejected shape -- spans
    // stored inside the result -- and it reddens under ASan the moment anyone reintroduces it.
    const ImportedModel model = fbxShapedModel();

    AnimationSourceResult copy;
    {
        const AnimationSourceResult original = animationCookChannels(model, 0);
        REQUIRE(original.ok);
        REQUIRE(original.channels.size() == 4);
        copy = original;  // a COPY, not a move: both must be independently valid
    }  // `original` is destroyed here, and so is every buffer it owned

    REQUIRE(copy.ok);
    REQUIRE(copy.channels.size() == 4);
    const AnimationCookResult cooked = cookByHand(copy, Guid{}, 0);
    REQUIRE((cooked.status == AnimationCookStatus::Ok));
    const CookedAnimationParseResult parsed = parseCookedAnimation(std::span<const std::byte>(cooked.bytes));
    REQUIRE((parsed.status == CookedAnimationStatus::Ok));
    REQUIRE(parsed.animation.channels.size() == 4);
    CHECK(parsed.animation.channels[0].targetNodeLocalId == 100U);
    CHECK(parsed.animation.channels[3].targetNodeLocalId == 407U);
}

TEST_CASE("animation_cook_source: warnings are capped at MAX_IMPORT_WARNINGS (AS13)") {
    ImportedModel model;
    model.nodes.push_back(node(1, INVALID_SUBASSET));
    model.roots.push_back(1);
    ImportedAnimation clip;
    // One real channel so the Structure predicate does not fire, then far more sentinel-targeted
    // channels than the cap allows.
    clip.channels.push_back(simpleChannel(1));
    for (std::size_t i = 0; i < MAX_IMPORT_WARNINGS + 5; ++i) {
        clip.channels.push_back(simpleChannel(INVALID_SUBASSET));
    }
    model.animations.push_back(std::move(clip));

    const AnimationSourceResult source = animationCookChannels(model, 0);
    REQUIRE(source.ok);
    CHECK(source.channels.size() == 1);
    CHECK(source.warnings.size() == MAX_IMPORT_WARNINGS);
}

TEST_CASE("animation_cook_source: the convenience composes byte-identically, and an error carries no bytes (AS14)") {
    SUBCASE("byte-identical to composing the three halves by hand") {
        const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/skinned.gltf");
        const ImportResult imported =
            importModel("skinned.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        REQUIRE((imported.status == ImportStatus::Ok));
        const Guid guid{0x0123456789abcdefULL, 0xfedcba9876543210ULL};

        for (std::uint32_t clipIndex = 0; clipIndex < 3; ++clipIndex) {
            const AnimationSourceResult source = animationCookChannels(imported.model, clipIndex);
            REQUIRE(source.ok);
            const AnimationCookResult byHand = cookByHand(source, guid, clipIndex);
            const AnimationCookResult composed = cookImportedAnimation(imported.model, clipIndex, guid);
            REQUIRE((byHand.status == AnimationCookStatus::Ok));
            REQUIRE((composed.status == AnimationCookStatus::Ok));
            CHECK(composed.bytes.size() == byHand.bytes.size());
            CHECK(composed.bytes == byHand.bytes);
            // The adapter's advisories ride out on the composed result and are absent from the
            // by-hand one, which is precisely the "no second implementation" the convenience buys.
            CHECK(anyWarningContains(composed.warnings, "has 3 animations"));
            CHECK_FALSE(anyWarningContains(byHand.warnings, "has 3 animations"));
        }
    }
    SUBCASE("an adapter error becomes {Invalid, error, {}, 0, {}}") {
        // A refusal carries NO warnings, and that is structural rather than a second policy: `fail`
        // returns a FRESH result, exactly as skeleton_cook_source.cpp's does, so anything the walk
        // had accumulated goes with it. Pinned here so the shape is a decision.
        //
        // The assertion is kept non-vacuous by asking the SAME model for a clip that succeeds: two
        // clips, so the multi-clip advisory demonstrably exists for this model, and clip 0 is a
        // Structure-depth shell, so asking for it refuses.
        ImportedModel model;
        model.nodes.push_back(node(0, INVALID_SUBASSET));
        model.roots.push_back(0);
        ImportedAnimation shell;
        shell.channels.push_back(channel(0, AnimationPath::Translation, AnimationInterpolation::Linear, {}, {}));
        model.animations.push_back(std::move(shell));
        ImportedAnimation second;
        second.channels.push_back(simpleChannel(0));
        model.animations.push_back(std::move(second));

        const AnimationSourceResult usable = animationCookChannels(model, 1);
        REQUIRE(usable.ok);
        REQUIRE(usable.warnings.size() == 1);  // the advisory really is produced for THIS model
        CHECK(anyWarningContains(usable.warnings, "has 2 animations"));

        const AnimationSourceResult source = animationCookChannels(model, 0);
        REQUIRE_FALSE(source.ok);
        CHECK(source.warnings.empty());

        const AnimationCookResult cooked = cookImportedAnimation(model, 0, Guid{});
        CHECK((cooked.status == AnimationCookStatus::Invalid));
        CHECK(cooked.message == source.error);
        CHECK(cooked.warnings.empty());
        CHECK(cooked.warningTotal == 0);
        CHECK(cooked.bytes.empty());
    }
}
