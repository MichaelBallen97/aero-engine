#pragma once
// engine::AudioSource (task 3.7.2): the reflected "this entity makes a sound" component. Pure
// reflected data, no .cpp. Registered as the SEVENTH built-in in engine/scene/src/transform.cpp.
//
// It is DECLARATIVE. It names a clip and describes how that clip should sound; nothing here knows
// what a voice, a mixer or a device is, and engine/scene links none of them. engine/scene_audio is
// the only code in the tree that sees both this component and engine::audio.
#include <aero/core/guid.hpp>            // Guid
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT, AERO_RANGE

#include <cstdint>
#include <type_traits>

namespace engine {

struct AERO_COMPONENT AudioSource {
    // The cooked .aerowave this entity plays. NIL == nothing to play, which is the ORDINARY state of
    // a freshly added component and is COUNTED, never warned -- the RenderView::unresolvedMeshes rule
    // verbatim: a WARN would fire once per session on correct behaviour.
    Guid clip{};

    float volume AERO_RANGE(0.0f, 1.0f) = 1.0f;

    // THE LITERAL 4.0f MIRRORS engine::audio::MAX_PITCH AND CANNOT BE WRITTEN AS A REFERENCE:
    // annotations.hpp requires AERO_RANGE's arguments to be numeric LITERALS and validates it. (The
    // 3.5.1 JP14 shape: a constant duplicated across a boundary needs a witness.)
    //
    // THE MIRROR TAKES TWO WITNESSES, ONE PER SIDE, AND NEITHER COVERS THE OTHER -- measured by
    // seeding each side separately, not assumed:
    //   * MAX_PITCH moving is caught by SA1, a static_assert in tests/scene_audio_test.cpp (the one
    //     translation unit that sees engine/scene AND engine/audio). A COMPILE failure.
    //   * THIS LITERAL moving is caught by tests/editor/inspector_test.cpp's AudioSource row, which
    //     reads rangeMax off the GENERATED meta. SA1 cannot see it: no C++ expression can read an
    //     AERO_RANGE argument, so the generated meta is structurally the only place it is visible.
    // Reading SA1 as a pin on this literal is the mistake to avoid -- it is a one-directional drift
    // tripwire, which is 3.7.1's A14 finding one task later.
    float pitch AERO_RANGE(0.0f, 4.0f) = 1.0f;

    // NO AERO_RANGE on either, and that is 1.3.3's D19 applied rather than an oversight: "min > 0
    // cannot be expressed by a two-sided bound, and inventing an upper limit would be false UI" --
    // the same reason Camera::nearPlane and farPlane carry none.
    float minDistance = 1.0f;   // world units; the gain is exactly 1 at or inside this
    float maxDistance = 50.0f;  // world units; the gain is exactly 0 at or beyond this

    bool loop = false;

    // DECLARATIVE, AND NEVER CLEARED BY ANYTHING BUT THE AUTHOR -- AnimationPlayer's contract
    // verbatim, for its reasons: a `finished` flag is DERIVED STATE in a serialized component, and
    // auto-clearing makes "paused at the end" and "stopped" indistinguishable on reload. A FINISHED
    // ONE-SHOT IS NOT RESTARTED; retrigger is false -> true.
    //
    // It defaults to TRUE because it is also the ONLY way a scene can make a sound in v0: there is no
    // script layer to call play() from until Phase 4, and an ambient loop is the shape this default is
    // right for. One-shot SFX triggering is the script API's job.
    bool playing = true;

    // FALSE == 2D: no pan, no distance attenuation, and it SURVIVES A WORLD WITH NO AudioListener.
    // That is the right default for UI and music, and the wrong one for a sound with a place.
    bool spatialize = true;

    bool operator==(const AudioSource&) const = default;
};

static_assert(std::is_trivially_copyable_v<AudioSource>);
static_assert(std::is_standard_layout_v<AudioSource>);
static_assert(std::is_aggregate_v<AudioSource>);
// 16 (Guid clip) + 4 + 4 + 4 + 4 (four floats) + 1 + 1 + 1 (three bools) = 35, rounded up to 40 by
// Guid's 8-alignment.
//
// THE FIVE PADDING BYTES ARE STATED RATHER THAN REMOVED (the MeshRenderer rule, same reasoning, same
// refusal): reordering to put the bools first packs to 40 ANYWAY, and would change JSON key order and
// inspector row order, which declaration order IS.
static_assert(sizeof(AudioSource) == 40);
static_assert(alignof(AudioSource) == 8);

}  // namespace engine
