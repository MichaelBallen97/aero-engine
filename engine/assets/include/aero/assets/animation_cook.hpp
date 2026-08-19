#pragma once
// Aero Engine — the animation cook (task 3.5.2): already-resolved plain data in, .aeroanim v1 bytes
// out. The PRODUCER half of the cooked animation container; cooked_animation.hpp is the FORMAT half,
// and a runtime that never cooks anything needs only that one.
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro,
// AND ZERO FLOATING-POINT ARITHMETIC. Every time and every value component travels std::bit_cast bit
// for bit through putF32. The only float operations anywhere in this file are COMPARISONS -- the
// strictly-increasing times check, and the durationSeconds fold, which is
// std::max(accumulator, lastTime) -- comparison-and-select, accumulator FIRST, in
// EMISSION order (docs/09 section 13.7, the mesh cook's model-box rule one format over). No hash
// container anywhere: ordering and duplicate detection are sorted vectors, so no output can depend
// on an iteration order.
//
// THE COOK CONVERTS NOTHING. No resampling, no key reduction, no quantization, no compression, no
// demotion of CUBICSPLINE to LINEAR, no unit scaling, no renormalization. There is no
// AnimationCookSettings type, for the same reason there is no MeshCookSettings and no
// TextureCookSettings: an empty settings struct is a shape that invites a field.
#include <aero/assets/cooked_animation.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::assets {

// One channel as the caller already resolved it: no model, no node tree, no editor type.
// `targetLocalId` is the SOURCE NODE's localId and it is written into the file VERBATIM -- see
// animation_cook_source.hpp for why that inverts the usual localId discipline.
struct AnimationCookChannel {
    std::uint32_t targetLocalId = 0;
    CookedAnimationPath path = CookedAnimationPath::Translation;
    CookedAnimationInterpolation interpolation = CookedAnimationInterpolation::Linear;
    std::span<const float> times;  // strictly increasing; EMPTY => this channel is DROPPED
    std::span<const Vec4> values;  // size == times.size() * cookedAnimationValuesPerKey(interpolation)
};

struct AnimationCookInput {
    Guid sourceGuid;                         // may be nil; nil is legal and deterministic
    std::uint32_t sourceAnimationIndex = 0;  // the POSITION in the model's animation list
    std::span<const AnimationCookChannel> channels;
};

// THREE-VALUED, and it is neither the mesh cook's nor the skeleton cook's -- derived rather than
// chosen (D9). A clip is per-channel independent, so dropping a rotation channel means "that joint's
// rotation is not animated", which is a complete and honest description of the artifact -- the mesh
// cook's argument. But a clip with NOTHING LEFT is the absence of animation rather than a degenerate
// animation, and playing it is a no-op that looks exactly like a bug -- .aeroskel's argument.
enum class AnimationCookStatus : std::uint8_t { Ok = 0, Truncated, Invalid };

struct AnimationCookResult {
    AnimationCookStatus status = AnimationCookStatus::Ok;
    std::string message;                // "" IFF Ok -- a Truncated result names its drops in one
                                        // sentence AND lists them below (the MeshCookResult rule)
    std::vector<std::string> warnings;  // capped at MAX_COOK_WARNINGS
    std::size_t warningTotal = 0;       // UNCAPPED (the MAX_REPORTED_PER_CATEGORY shape)
    std::vector<std::byte> bytes;       // EMPTY IFF Invalid
};

// Validate, canonicalize and emit. Refusals, each with a message naming the defect: an empty channel
// list; every channel dropped; either count cap exceeded; an unknown path or interpolation code; a
// valueCount that is not keyCount * the multiplier; times that are not strictly increasing; and a
// duplicate (targetNodeLocalId, path) pair, whose message names the node id and the path label --
// glTF section 3.6.1 makes that pair a MUST NOT, and the ambiguity is WHICH motion is the truth,
// which has no defensible answer. A refusal can be relaxed later; a silent drop cannot be tightened.
//
// Every non-Invalid result's bytes parse Ok through parseCookedAnimation, with every field equal to
// what went in, in the canonical order.
[[nodiscard]] AnimationCookResult cookAnimation(const AnimationCookInput& input);

}  // namespace engine::assets
