#pragma once
// Aero Engine — the mixer (task 3.7.2). THE REALTIME SIDE of the audio layer: it owns the voice pool,
// the published clip table and the block render, and it is a PLAIN OBJECT — no device, no thread, no
// timing anywhere in its interface. That is D18's whole point, and it is what makes every MX case in
// tests/audio_mixer_test.cpp tier-0: a test builds one, hands it commands and calls render(), with no
// device, no thread and no clock in sight.
//
// ============================================================================================
// THREAD OWNERSHIP. This table is normative and is written down exactly once, here.
//
//   Owned by the GAME thread                       | Owned by the AUDIO thread
//   -----------------------------------------------+-------------------------------------------
//   the slot free list and every slot's generation | every Voice's cursor, gains and playing state
//   the vector that OWNS the AudioClips            | the fixed AudioClip* table it reads
//   the Guid -> ClipHandle sorted map              | the current ListenerPose and master volume
//   the command ring's WRITE index                 | the command ring's READ index
//   the retire ring's READ index                   | the retire ring's WRITE index
//
// (The free list, the owning vector, the sorted map and both ring indices other than this class's
// own live in AudioSystem; what is listed here is the whole picture, because half a picture is how
// the next change goes wrong.)
//
// EVERY MEMBER BELOW IS LABELLED WITH THE THREAD THAT MAY CALL IT. The audio-thread members
// ALLOCATE NOTHING, LOCK NOTHING, LOG NOTHING AND THROW NOTHING. clip.hpp already establishes the
// layer's posture on the third of those: "NOTHING IN THIS FILE LOGS."
// ============================================================================================

#include <aero/audio/clip.hpp>
#include <aero/audio/spatial.hpp>
#include <aero/core/handle.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::audio {

// 64 simultaneous voices at 48 kHz stereo is ~6 M sample-writes per second, which is nothing, and
// 64 * sizeof(Voice) is a few kilobytes. Unity's default real (non-virtual) voice count is 32.
//
// VOICE STEALING IS NOT IN v0: play() past the cap refuses, counts, and latches ONE warning per
// AudioSystem lifetime. Refusing predictably beats stealing unpredictably when nothing yet has a
// priority to steal by; priority is a named handoff.
inline constexpr std::uint32_t MAX_VOICES = 64;

inline constexpr std::uint32_t MAX_CLIPS = 256;

// MIRRORED as a LITERAL by engine::AudioSource::pitch's AERO_RANGE(0.0f, 4.0f), because
// annotations.hpp requires AERO_RANGE's arguments to be numeric literals and validates it. The mirror
// is pinned by SA1 -- a static_assert in tests/scene_audio_test.cpp, the one TU that sees engine/scene
// AND engine/audio -- so drift is a COMPILE failure rather than a test failure.
inline constexpr float MAX_PITCH = 4.0F;
// 0 == a live but SILENT voice, parked on the frame it reached (AnimationPlayer::speed == 0 is pause
// without clearing `playing`, and this is the same shape). A negative pitch clamps to 0: reverse
// playback needs a signed cursor and its own loop-wrap rule, and is a named handoff.
inline constexpr float MIN_PITCH = 0.0F;

struct ClipTag {};
struct VoiceTag {};

// ClipHandle::generation is ALWAYS 1 in v0, because THERE IS NO CLIP RETIREMENT (D7): every handshake
// this task could ship cannot complete while the device is stopped, since the audio thread is the only
// party that can acknowledge. Clips live for the AudioSystem's lifetime and ~AudioSystem frees them
// all. Stating the always-1 here is what makes adding retirement later PURELY ADDITIVE rather than a
// semantic change.
using ClipHandle = Handle<ClipTag>;
using VoiceHandle = Handle<VoiceTag>;

// What a voice is told, in one POD. Every field is sanitised on the way in (D9's first defence, at
// the AudioSystem API boundary), so nothing here can carry a NaN into the render.
struct VoiceParams {
    Vec3 position{};
    float volume = 1.0F;  // clamped [0, 1]
    float pitch = 1.0F;   // clamped [MIN_PITCH, MAX_PITCH]
    float minDistance = 1.0F;
    float maxDistance = 50.0F;
    bool loop = false;
    bool spatialize = true;

    // Defaulted == so the bridge's coalescing compares the WHOLE struct rather than field by field.
    // A field-by-field comparison is exactly where a future appended field gets forgotten -- 3.6.2's
    // positional-brace-init lesson in a different costume.
    bool operator==(const VoiceParams&) const = default;
};

// One instruction, game thread -> audio thread. A plain trivially-copyable value: the ring stores it
// by value and nothing here owns anything.
struct AudioCommand {
    enum class Kind : std::uint8_t { Start, Stop, StopAll, SetParams, SetListener, SetMasterVolume };
    Kind kind = Kind::StopAll;
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    ClipHandle clip{};
    VoiceParams params{};
    ListenerPose listener{};
    float masterVolume = 1.0F;
};

// One finished voice, audio thread -> game thread. It carries the GENERATION as well as the slot, so
// the game thread can tell a retirement of the voice it is holding from a retirement of a voice that
// occupied the same slot two allocations ago.
struct RetiredVoice {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
};

class AudioMixer {
public:
    AudioMixer() = default;
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&) = delete;
    AudioMixer& operator=(AudioMixer&&) = delete;
    ~AudioMixer() = default;

    // ---- GAME THREAD ------------------------------------------------------------------------
    // Writes table[index] and then release-stores the count, so the audio thread's acquire-load of
    // the count is what publishes the pointer. `index` must be the next unpublished slot; anything
    // else is refused. Returns false past MAX_CLIPS or on an out-of-order index.
    bool publishClip(std::uint32_t index, const AudioClip* clip) noexcept;

    // The audio thread PUSHES retirements; AudioSystem::service() POPS them here. False when empty.
    [[nodiscard]] bool popRetired(std::uint32_t& slot, std::uint32_t& generation) noexcept;

    // ---- AUDIO THREAD -----------------------------------------------------------------------
    // Applies one drained command. Allocates nothing, locks nothing, logs nothing, throws nothing.
    void applyCommand(const AudioCommand& command) noexcept;

    // Renders ONE block. `output` is interleaved f32 and is UNINITIALISED on entry; this function
    // writes EVERY element on EVERY path, silence included.
    void render(std::span<float> output, std::uint32_t channels, std::uint32_t sampleRate) noexcept;

    // ---- DIAGNOSTICS: written by the audio thread, read from either, RELAXED both ways. ------
    // They are a READOUT, never a synchronisation point; nothing's correctness depends on a value.
    [[nodiscard]] std::uint32_t activeVoices() const noexcept;
    [[nodiscard]] std::uint64_t framesRendered() const noexcept;
    [[nodiscard]] std::uint64_t callbacksCompleted() const noexcept;

private:
    // THE VOICE POOL IS A FIXED std::array AND DELIBERATELY NOT A SlotMap (D6/F14).
    //
    // SlotMap is exactly the right SHAPE -- {index, generation} handles, stable across inserts and
    // removes -- and exactly the wrong STORAGE: slot_map.hpp:36 says insert "May allocate (grows the
    // slot array)", and its storage is a std::vector<Slot>. THE AUDIO THREAD READS THAT STORAGE, so a
    // reallocation on the game thread mid-block is a USE-AFTER-FREE ON A REALTIME THREAD that no care
    // at the call site can fix. Do not "simplify" this into a SlotMap.
    struct Voice {
        std::uint32_t generation = 0;  // the generation of the voice currently installed in this slot
        ClipHandle clip{};
        VoiceParams params{};
        std::uint64_t cursor = 0;  // 32.32 fixed point; see mixer.cpp for why a float is a defect
        bool active = false;
    };

    std::array<Voice, MAX_VOICES> voices{};

    // The clip table. A FIXED array is what makes the release/acquire publication sound; a
    // std::vector of pointers would reallocate under the reader.
    std::array<const AudioClip*, MAX_CLIPS> clipTable{};
    std::atomic<std::uint32_t> clipCount{0};

    // Audio-thread-owned globals, set by SetListener / SetMasterVolume commands.
    ListenerPose listener{};
    float masterVolume = 1.0F;

    // THE RETIRE RING'S CAPACITY, WITH ITS PROOF BESIDE IT.
    //
    // A slot is retired AT MOST ONCE PER ALLOCATION, and it cannot be re-allocated until the game
    // thread has drained its retirement. Therefore at most MAX_VOICES (64) retirements are
    // outstanding at any instant. Capacity is 128 -- the next power of two above 65, so the index is
    // a single & rather than a modulo.
    //
    // A CAPACITY NUMBER WITH NO ARGUMENT BESIDE IT IS EXACTLY THE NUMBER THAT ROTS.
    static constexpr std::uint32_t RETIRE_RING_CAPACITY = 128;
    static_assert((RETIRE_RING_CAPACITY & (RETIRE_RING_CAPACITY - 1)) == 0, "capacity must be a power of two");
    static_assert(RETIRE_RING_CAPACITY > MAX_VOICES, "the proof above requires capacity > MAX_VOICES");

    std::array<RetiredVoice, RETIRE_RING_CAPACITY> retireRing{};
    std::atomic<std::uint32_t> retireHead{0};  // game thread writes
    std::atomic<std::uint32_t> retireTail{0};  // audio thread writes

    std::atomic<std::uint32_t> activeCount{0};
    std::atomic<std::uint64_t> frameTotal{0};
    std::atomic<std::uint64_t> callbackTotal{0};

    // Audio thread only. Pushes onto the retire ring; silently drops if it is somehow full, because
    // the proof above says it cannot be and a realtime thread has nothing better to do about it.
    void retire(std::uint32_t slot, std::uint32_t generation) noexcept;

    // Audio thread only. Accumulates ONE voice into the block. Returns false when the voice reached
    // the end of a non-looping clip, in which case render() retires it -- at the block's end, never
    // mid-walk.
    bool mixVoice(Voice& voice, const AudioClip& clip, std::span<float> output, std::uint32_t channels,
                  std::size_t frames) noexcept;
};

}  // namespace engine::audio
