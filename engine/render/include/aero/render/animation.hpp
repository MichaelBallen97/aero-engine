#pragma once
// Aero Engine — the clip sampler (task 3.5.2): bind once, sample per frame, write JointPoses.
// assets::CookedAnimation IS the runtime clip -- no mirror struct, exactly as assets::CookedSkeleton
// is the runtime skeleton (3.5.1's D6). aero_render already links PUBLIC aero::assets (3.4.1's
// edge), so naming those types here adds no link line, no include directory and no dependency.
//
// PURE and GPU-FREE: no rhi type, no device call, no allocation, no logging, no recursion, no
// per-OS macro -- skinning.hpp's own rule, restated. This header is the vocabulary only.
//
// THE SPLIT: engine/scene owns "what time is it" (engine::AnimationPlayer and
// advanceAnimationPlayer); engine/render owns "what pose is that". sampleAnimation takes a bare
// float and neither knows nor cares whether the caller looped.
#include <aero/assets/cooked_animation.hpp>
#include <aero/assets/cooked_skeleton.hpp>
#include <aero/render/skinning.hpp>  // JointPose -- the seam 3.5.1 declared for this task by name

#include <cstdint>
#include <span>

namespace engine::render {

// bindAnimation REPORTS, it never refuses (D12). None of these four is an error: a channel targeting
// a node that is not in the skeleton is a NORMAL outcome -- glTF clips routinely animate camera and
// mesh nodes alongside joints -- and it is skipped at sample time. A sourceGuid mismatch is reported
// because it is almost always a mistake and never PROVABLY one: two cooks of the same asset agree,
// and a deliberate cross-rig bind is exactly what retargeting is, which v1 neither supports nor
// forbids. The CALLER decides what to do with the numbers.
struct AnimationBindStats {
    std::uint32_t channelCount = 0;
    std::uint32_t boundChannels = 0;
    std::uint32_t unboundChannels = 0;
    bool sourceGuidMatches = false;
};

// Bind ONCE, at load. Fills one entry per clip channel: the skeleton's JOINT RECORD INDEX whose
// sourceNodeLocalId equals that channel's targetNodeLocalId, or assets::COOKED_SKELETON_INVALID_INDEX
// when no record matches. out.size() must equal clip.channels.size() (debug-asserted; clamped to the
// shorter in release -- bindPose's exact posture, because a per-frame path's neighbour must never
// turn a caller's bookkeeping slip into a read).
//
// A LINEAR SCAN per channel over the skeleton's records, deliberately: bounded by 4096 x 1024
// comparisons in the worst legal case, ONCE, at load. A sorted index would be an allocation, and
// requiring the format to store records sorted by sourceNodeLocalId would fight section 12.4's
// parents-before-children order, which computeJointPalette's single forward pass depends on.
[[nodiscard]] AnimationBindStats bindAnimation(const assets::CookedAnimation& clip,
                                               const assets::CookedSkeleton& skeleton, std::span<std::uint32_t> out);

// Sample per frame. Writes ONLY the T, R or S member each bound channel drives, member-wise, and
// touches nothing else -- the caller pre-fills `pose` with bindPose(). THAT CONTRACT IS THE ENTIRE
// REASON docs/09 section 12.3 stores bind LOCALS as TRS rather than baked global matrices, and it is
// discharged here rather than merely predicted.
//
// Unbound channels and joint indices past `pose` are skipped. `timeSeconds` is clip-local and needs
// no clamping by the caller: glTF section 3.6.2's clamp rule handles anything, INCLUDING a negative
// or non-finite value. Allocation-free, recursion-free, logging-free; the binding span is
// debug-asserted and release-clamped, and a joint index past `pose` is skipped rather than asserted
// (it is a normal outcome of a rig narrower than the clip, not a caller slip). The per-channel key
// search is a binary search over that channel's times slice -- a monotonic-playback cursor cache
// would need per-instance mutable state, which is the animation-instance type v2's graph work
// introduces.
void sampleAnimation(const assets::CookedAnimation& clip, std::span<const std::uint32_t> binding, float timeSeconds,
                     std::span<JointPose> pose);

}  // namespace engine::render
