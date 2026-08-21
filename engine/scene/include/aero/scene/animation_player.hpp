#pragma once
// Aero Engine — engine::AnimationPlayer (task 3.5.2): the reflected playback-state component and the
// single definition of "what time is it". Registered as the SIXTH built-in in
// engine/scene/src/transform.cpp, so every World the editor makes can inspect and edit it, and
// engine/scene_serialize saves it -- BOTH, or the component is editable and unsaved.
//
// IT CARRIES NO CLIP REFERENCE, AND THAT IS A DECISION RATHER THAN AN OMISSION. The spelling of a
// scene -> asset reference is task 3.1.5's by its own task text and by four consecutive handoffs; a
// clip reference alone cannot produce a picture (playing a clip needs a mesh, a skeleton AND a clip,
// and a scene entity can name none of the three), so adding one here would only pre-empt that
// decision with a spelling 3.1.5 would have to reconcile. THE REVERSAL IS ONE APPENDED FIELD: when
// scene -> asset references land, this struct gains its clip reference AFTER `playing`, in that
// task's chosen spelling, and every scene file written before then still loads -- docs/09 section
// 2.3's missing-key rule is "silent, the target field is left untouched".
//
// REFLECTION: every field is float or bool, both inside reflect-gen's 18-CXTypeKind whitelist and
// both already rendered by inspector_panel.cpp (FieldKind::Float at :278, FieldKind::Bool at :221),
// so the inspector control the deliverable asks for is the reflection spine doing its job. A bespoke
// panel would be a regression against ADR-004, not an addition. NO enum, deliberately: v1 has
// exactly two looping behaviours -- wrap, or clamp-and-hold -- which a bool spells completely, and
// growing reflect-gen's subset is a task rather than a field.
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT

#include <cstddef>
#include <type_traits>

namespace engine {

struct AERO_COMPONENT AnimationPlayer {
    float time = 0.0F;  // clip-local seconds; advanceAnimationPlayer keeps it in [0, duration]
    // NO AERO_RANGE, and that is 1.3.3's D19 applied rather than an oversight: Camera's
    // nearPlane/farPlane carry none because "min > 0 cannot be expressed by a two-sided bound, and
    // inventing an upper limit would be false UI". EVERY upper bound on playback speed is invented.
    // May be negative (reverse) and zero (pause without clearing `playing`).
    float speed = 1.0F;
    bool loop = true;
    bool playing = true;

    bool operator==(const AnimationPlayer&) const = default;
};

static_assert(std::is_trivially_copyable_v<AnimationPlayer>);
static_assert(std::is_standard_layout_v<AnimationPlayer>);
static_assert(std::is_aggregate_v<AnimationPlayer>);
static_assert(sizeof(AnimationPlayer) == 12);  // 4 + 4 + 1 + 1 + 2 padding

// THE CLOCK, and there is exactly one of it. A free function beside the component with one extrinsic
// parameter -- the projectionMatrix(Camera, aspect) / viewMatrix(World, Entity) /
// localMatrix(Transform) shape, with three precedents in this directory. `durationSeconds` is
// extrinsic because it belongs to the CLIP, which nothing in a scene can name until task 3.1.5.
//
// In order, and the order is the contract:
//   1. !playing        -> return; a paused player's time is untouched, so pause/resume is exact
//   2. !(duration > 0) -> time = 0; ONE predicate covering zero, negative and NaN duration. A
//                         zero-duration clip is a static pose and sampling it at 0 is correct
//   3. time += speed * dt
//   4. !isfinite(time) -> time = 0; an infinite or NaN dt or speed cannot poison the component
//   5. loop            -> fmod against duration, then += duration if negative. fmod is EXACT in
//                         IEEE-754, so a huge dt costs one call rather than an unbounded subtract
//                         loop, and reverse playback wraps to the END rather than sticking at zero
//   6. !loop           -> clamp into [0, duration]; it holds on the last frame (or the first, in
//                         reverse) and stays there
//
// `playing` IS NEVER CLEARED BY THE CLOCK and there is no `finished` flag. Both were considered: a
// finished bool is derived state in a serialized component, and auto-clearing `playing` makes
// "paused at the end" and "stopped" indistinguishable on reload. Animation EVENTS are the feature
// that actually wants this, and they belong to the v2 graph work.
void advanceAnimationPlayer(AnimationPlayer& player, float deltaSeconds, float durationSeconds);

}  // namespace engine
