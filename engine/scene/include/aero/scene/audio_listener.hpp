#pragma once
// engine::AudioListener (task 3.7.2): the ear. Pure reflected data, no .cpp. Registered as the
// EIGHTH built-in in engine/scene/src/transform.cpp.
//
// POSITION AND ORIENTATION COME FROM THE ENTITY'S Transform (worldMatrix), SO THIS COMPONENT STORES
// NEITHER -- the light.hpp D6 rule, unchanged: a directional light stores no direction either.
// engine/scene_audio builds the listener basis from worldMatrix's columns, normalising each, which is
// what makes a scaled listener entity behave identically to an unscaled one.
//
// A world may legally hold more than one; the LOWEST ENTITY INDEX wins, and both zero and more-than-
// one are each a latched WARN in the bridge -- the Camera / DirectionalLight D5 rule, so a reader who
// knows one of the three knows all three.
//
// It is a separate header rather than a second struct in audio_source.hpp for two reasons: light.hpp
// holds two components because they are the same KIND of thing, and a source and a listener are not;
// and <aero/scene/audio.hpp> beside <aero/audio/audio.hpp> in one translation unit is a confusion
// worth one extra file to avoid.
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT, AERO_RANGE

#include <type_traits>

namespace engine {

struct AERO_COMPONENT AudioListener {
    // ONE field, and it is a REAL one rather than padding to avoid an empty struct: this is the
    // listener's own gain, which is where Unity puts the global volume too. It MULTIPLIES with
    // AudioSystem's master volume, and the two are separate because one is AUTHORED INTO THE SCENE and
    // the other is a RUNTIME KNOB (mute, ducking, a settings slider).
    float volume AERO_RANGE(0.0f, 1.0f) = 1.0f;

    bool operator==(const AudioListener&) const = default;
};

static_assert(std::is_trivially_copyable_v<AudioListener>);
static_assert(std::is_standard_layout_v<AudioListener>);
static_assert(std::is_aggregate_v<AudioListener>);
static_assert(sizeof(AudioListener) == sizeof(float));  // no padding
static_assert(alignof(AudioListener) == alignof(float));

}  // namespace engine
