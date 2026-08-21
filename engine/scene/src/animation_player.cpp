// engine/scene/src/animation_player.cpp — task 3.5.2: the playback clock, and nothing else. The
// FOURTH TU in engine/scene, and the first that knows neither World nor the internal seam: it reads
// and writes one component and one extrinsic float, so it needs no registry, no entity and no
// matrices.
//
// No Tracy zone, deliberately (camera.cpp's rule applied): a zone belongs on viewMatrix's inverse
// plus ancestor walk, not on a handful of float operations called once per player per frame, where
// it would cost more than the function and flood a capture with per-entity scopes.
//
// Every branch here is TOTAL for every float the caller can hand it -- an infinite dt, a NaN speed
// and a negative duration all land on a defined `time`, because this component is serialized and a
// poisoned field survives a save.

#include <aero/scene/animation_player.hpp>

#include <algorithm>  // std::clamp
#include <cmath>      // std::fmod, std::isfinite

namespace engine {

void advanceAnimationPlayer(AnimationPlayer& player, float deltaSeconds, float durationSeconds) {
    if (!player.playing) {
        return;  // 1. a paused player's time is BIT-identical across the call, so resume is exact
    }
    if (!(durationSeconds > 0.0F)) {
        player.time = 0.0F;  // 2. one predicate covers zero, negative and NaN
        return;
    }
    player.time += player.speed * deltaSeconds;  // 3.
    if (!std::isfinite(player.time)) {
        player.time = 0.0F;  // 4. a non-finite dt or speed cannot poison the component permanently
        return;
    }
    if (player.loop) {
        // 5. fmod is exact in IEEE-754, so a delta spanning a million periods costs ONE call.
        player.time = std::fmod(player.time, durationSeconds);
        if (player.time < 0.0F) {
            player.time += durationSeconds;  // reverse playback wraps to the END, never sticks at 0
        }
        return;
    }
    player.time = std::clamp(player.time, 0.0F, durationSeconds);  // 6. clamp and HOLD
}

}  // namespace engine
