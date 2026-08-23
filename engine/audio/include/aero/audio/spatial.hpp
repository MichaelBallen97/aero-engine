#pragma once
// Aero Engine — the spatializer (task 3.7.2): a linear distance rolloff and a constant-power azimuth
// pan, as PURE FUNCTIONS OVER PURE DATA. No state, no allocation, no logging, no rhi type, no device,
// no clock. That is the culling.{hpp,cpp} mold, and it is why every case in
// tests/audio_spatial_test.cpp runs in every configuration this project builds, with AERO_REQUIRE_GPU
// set and unset.
//
// A RECORDED DEVIATION FROM THE TASK TEXT. docs/tasks/phase-3.md's 3.7.2 deliverable says "basic 3D
// panning via the miniaudio spatializer". ma_spatializer IS available at the pinned miniaudio
// 0.11.25, so this is a choice rather than a workaround: miniaudio.h is PRIVATE to aero_platform, so
// reaching it would put the voice table, the mixer and the listener in engine/platform beside
// miniaudio.h -- inverting ADR-006's own layer diagram and voiding the no-vcpkg property
// engine/audio/CMakeLists.txt spends a paragraph establishing and task 3.7.3 exists to guard.
//
// THE MATH IS DELIBERATELY SIMPLE AND THE SIMPLICITY IS THE POINT. A linear rolloff and a
// constant-power pan are exactly what "v0 spatialization" means; HRTF, occlusion, reverb zones,
// Doppler and inverse/logarithmic rolloff modes are named handoffs, not omissions.

#include <aero/core/math.hpp>

#include <array>
#include <cstdint>

namespace engine::audio {

// The widest output this mixer will write. It HAPPENS to equal assets::MAX_COOKED_AUDIO_CHANNELS,
// and that is a COINCIDENCE rather than a relationship: a future 16-channel OUTPUT does not widen the
// .aerowave container, and a future 16-channel container does not widen this. Deriving one from the
// other by #include would say the two numbers must move together, which is false. The equality is
// pinned by SP1 instead -- the honest form of the same statement, and the JP14 shape this project
// already uses for a constant duplicated across a boundary.
inline constexpr std::uint32_t MAX_AUDIO_OUTPUT_CHANNELS = 8;

// D13: the pan spans channels 0 and 1 and nothing else. Surround placement needs a channel map in
// .aerowave -- a formatVersion bump, bundled per section 14's own rule, and a named handoff.
inline constexpr std::uint32_t SPATIAL_PAN_CHANNELS = 2;

// The listener, in WORLD space, as an orthonormal basis plus a position. engine/scene_audio builds it
// from worldMatrix()'s columns; NOTHING HERE KNOWS WHAT A WORLD IS.
//
// `valid` defaults to FALSE, and that default is load-bearing: a default-constructed pose means THERE
// IS NO LISTENER, which is D23's whole mechanism (spatialized voices go silent, non-spatialized ones
// keep playing) and must not be reachable by accident.
struct ListenerPose {
    Vec3 position{};
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};  // -Z forward (ADR-005)

    // The engine::AudioListener component's own gain. IT IS DELIBERATELY NOT APPLIED BY
    // computeSpatialGains: that function answers "what does this source sound like from that
    // listener", and the listener's gain and the master volume are MIX-WIDE scalars the mixer applies
    // uniformly to spatialized AND non-spatialized voices. Folding one of them in here would make the
    // mixer's passthrough path need its own copy -- two places for one number. Said here because the
    // natural reading of this field is that the function below uses it.
    float volume = 1.0F;

    bool valid = false;  // FALSE == no listener in the world (D23)
};

// One source's spatial input. `volume` IS applied here (it is per-source, unlike the two above).
struct SpatialParams {
    Vec3 position{};
    float minDistance = 1.0F;
    float maxDistance = 50.0F;
    float volume = 1.0F;
};

// Per-output-channel linear gains. A plain array by value: no allocation, trivially copyable, and
// small enough that passing it around costs nothing on any lane.
struct ChannelGains {
    std::array<float, MAX_AUDIO_OUTPUT_CHANNELS> gain{};
};

// EXACTLY 1 at distance <= minDistance and EXACTLY 0 at distance >= maxDistance, linear between --
// both ends are exact in the expression as written, so SP2 asserts them with == and NO epsilon.
//
// maxDistance <= minDistance is NOT an error: 1 inside, 0 outside, WITH NO DIVISION PERFORMED on that
// arm. Any non-finite input answers 0.
[[nodiscard]] float distanceGain(float distance, float minDistance, float maxDistance) noexcept;

// Constant-power azimuth pan: gain[0] = cos((x+1) * PI/4), gain[1] = sin((x+1) * PI/4), so
// gain[0]^2 + gain[1]^2 == 1 for every x in [-1, 1]. x is clamped to [-1, 1] here rather than at the
// call site. Channels 2.. stay 0. outputChannels == 1 collapses to a single gain of 1, because no pan
// is representable in one channel; outputChannels == 0 returns all zeros.
[[nodiscard]] ChannelGains panGains(float x, std::uint32_t outputChannels) noexcept;

// THE composition, and the one entry point a mixer calls. Returns ALL-ZERO gains when the listener is
// invalid, when the distance is at or beyond maxDistance, or when ANY input component is non-finite.
//
// THE FINITENESS CHECK IS ONE PREDICATE OVER THE WHOLE INPUT, NEVER PER-TERM (D9). Task 3.5.2's
// code-review round found a NaN reaching the GPU because a value survived sanitising ON ITS OWN:
// `NaN > 0` is false and `finite / inf` is 0, but `inf * 0` and `NaN * 0` are both NaN. A chain of
// per-use guards is exactly the shape that let it through. SP10's arms are what prove no term slipped
// past this one.
//
// FRONT AND BACK AT EQUAL DISTANCE PRODUCE IDENTICAL GAINS. That is azimuth-only panning working as
// designed (D13), not a defect: distinguishing them needs either more than two output channels or an
// HRTF, and both are named handoffs. SP14 asserts it so it reads as a decision rather than a surprise.
[[nodiscard]] ChannelGains computeSpatialGains(const ListenerPose& listener, const SpatialParams& source,
                                               std::uint32_t outputChannels) noexcept;

}  // namespace engine::audio
