// Aero Engine — the audio no-vcpkg-boundary COMPILE-TIME guard (task 3.7.3; ADR-006; epic 3.7's
// Definition of Done: "no miniaudio type is public (guard-enforced)").
//
// THIS FILE ASSERTS BY EXISTING. It is not a doctest suite and has no TEST_CASE: the assertion is
// that it COMPILES. Its target (aero_audio_boundary_probe, tests/CMakeLists.txt) links ONLY
// aero::audio -- which links NO vcpkg package at all, and whose PUBLIC deps aero::core/aero::assets
// propagate no vcpkg header -- so vcpkg's shared per-triplet include/ root never reaches this
// compile line and miniaudio (or any third-party header) is genuinely unreachable here. If any
// public audio header ever starts including <miniaudio.h>, THIS TU fails to compile the moment the
// leak is written.
//
// THIS PROBE IS THE ONLY ALL-CONFIGURATION COMPILE-TIME ENFORCEMENT THE AUDIO LAYER HAS. aero_audio
// itself links aero::profiling PRIVATE, and in the *-release presets that INTERFACE library carries
// Tracy::TracyClient -- and with it the whole shared vcpkg include root -- onto aero_audio's OWN
// compile line, so a stray include inside engine/audio compiles clean there and fails only in
// Debug. This target links no profiling, so its line is vcpkg-free in every preset.
//
// aero_tests CANNOT do this: it links doctest AND SDL3 directly and inherits the whole shared vcpkg
// include root, so <miniaudio.h> would resolve there regardless of what aero_audio links (risk R12,
// docs/08-risks.md). The textual guard (.github/scripts/check-audio-boundary.sh) reaches the
// sources and the CMakeLists; THIS probe holds the public-header compile boundary.
//
// KEEP THIS TARGET DEPENDENCY-FREE -- see tests/CMakeLists.txt and
// .github/scripts/check-boundary-probes.sh, which now enforces exactly that.

#include <aero/audio/audio.hpp>  // the umbrella: clip + mixer + spatial + system

#include <cstdint>
#include <type_traits>

// ---- clip.hpp (task 3.7.1) -- the runtime clip: move-only owner, statuses, no decoder. ----------
static_assert(static_cast<std::uint8_t>(engine::audio::AudioClipLoadStatus::Ok) == 0U);
static_assert(noexcept(engine::audio::audioClipLoadStatusLabel(engine::audio::AudioClipLoadStatus::Ok)));
static_assert(!std::is_copy_constructible_v<engine::audio::AudioClip>);  // owns AND views: copy is deleted
static_assert(std::is_move_constructible_v<engine::audio::AudioClip>);
static_assert(std::is_default_constructible_v<engine::audio::AudioClipLoadResult>);

// ---- spatial.hpp (task 3.7.2) -- pure functions over pure data; the pinned defaults. ------------
static_assert(engine::audio::MAX_AUDIO_OUTPUT_CHANNELS == 8U);
static_assert(engine::audio::SPATIAL_PAN_CHANNELS == 2U);
static_assert(!engine::audio::ListenerPose{}.valid);  // no-listener default is load-bearing (D23)
static_assert(engine::audio::ListenerPose{}.volume == 1.0F);
static_assert(engine::audio::ListenerPose{}.forward.z == -1.0F);  // -Z forward (ADR-005)
static_assert(engine::audio::SpatialParams{}.minDistance == 1.0F);
static_assert(engine::audio::SpatialParams{}.maxDistance == 50.0F);
static_assert(std::is_trivially_copyable_v<engine::audio::ChannelGains>);
static_assert(noexcept(engine::audio::distanceGain(0.0F, 0.0F, 0.0F)));
static_assert(noexcept(engine::audio::panGains(0.0F, 2U)));

// ---- mixer.hpp (task 3.7.2) -- handles, params, commands; the realtime object is unmovable. -----
static_assert(engine::audio::MAX_VOICES == 64U);
static_assert(engine::audio::MAX_CLIPS == 256U);
static_assert(engine::audio::MAX_PITCH == 4.0F);
static_assert(engine::audio::MIN_PITCH == 0.0F);
static_assert(!engine::audio::ClipHandle{}.valid());
static_assert(!engine::audio::VoiceHandle{}.valid());
static_assert(engine::audio::VoiceParams{}.volume == 1.0F);
static_assert(engine::audio::VoiceParams{}.pitch == 1.0F);
static_assert(engine::audio::VoiceParams{}.spatialize);
static_assert(!engine::audio::VoiceParams{}.loop);
static_assert(engine::audio::VoiceParams{} == engine::audio::VoiceParams{});  // the == the bridge coalesces on
static_assert(engine::audio::AudioCommand{}.kind == engine::audio::AudioCommand::Kind::StopAll);
static_assert(std::is_trivially_copyable_v<engine::audio::AudioCommand>);
static_assert(!std::is_copy_constructible_v<engine::audio::AudioMixer>);
static_assert(!std::is_move_constructible_v<engine::audio::AudioMixer>);  // audio thread reads it in place

// ---- system.hpp (task 3.7.2) -- the public surface: non-copyable AND non-movable. ---------------
static_assert(engine::audio::AudioSystemConfig{}.masterVolume == 1.0F);
static_assert(engine::audio::AudioStats{}.activeVoices == 0U);
static_assert(engine::audio::AudioStats{}.framesRendered == 0U);
static_assert(!std::is_copy_constructible_v<engine::audio::AudioSystem>);
static_assert(!std::is_move_constructible_v<engine::audio::AudioSystem>);  // a device holds `this` as renderUser
