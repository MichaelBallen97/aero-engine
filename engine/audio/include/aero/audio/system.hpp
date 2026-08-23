#pragma once
// Aero Engine — AudioSystem (task 3.7.2): THE PUBLIC AUDIO SURFACE (ADR-006), engine types only, on
// every signature, in every direction. It owns the clips, the voice slots and both SPSC rings, and it
// is the only thing in this layer a game or an editor talks to.
//
// ============================================================================================
// NON-COPYABLE **AND NON-MOVABLE**, and the second half is the load-bearing one: a
// platform::AudioDevice holds `this` as its renderUser, so a move would leave a REALTIME THREAD
// CALLING INTO A MOVED-FROM OBJECT. That is 0.3.3's D9 one layer up, and the same hazard AudioClip's
// deleted copy exists to make unspellable. SY1 asserts all four special members.
//
// THE LIFETIME RULE IS ONE SENTENCE: **DECLARE THE DEVICE AFTER THE SYSTEM.** ma_device_uninit stops
// the stream and JOINS the audio thread before it returns, so ordinary reverse-order destruction
// tears the device down first, EVERY TIME, WITH NO FLAG AND NO HANDSHAKE. It is written here, in
// audio.hpp's umbrella comment, and beside the two declarations in samples/phase-3-audio/main.cpp,
// because it is the one rule a consumer can get wrong and the symptom is a use-after-free on a
// thread nobody is looking at.
//
// THE THREADING CONTRACT, per datum:
//   command ring  std::array<AudioCommand, 1024>, head/tail as std::atomic<std::uint32_t>
//                   writer (game):  write the slot ; tail.store(tail + 1, release)
//                   reader (audio): tail.load(acquire) ; read slots ; head.store(new, release)
//   retire ring   lives in AudioMixer; its capacity proof is beside the number, in mixer.hpp
//                   writer (audio) ; reader (game)
//   clip table    publishClip writes table[i] ; clipCount.store(i + 1, release)
//                   audio thread: clipCount.load(acquire) ONCE per block, reads no pointer at or
//                   past it
//   diagnostics   std::atomic<std::uint64_t>, RELAXED BOTH WAYS -- a readout, never a
//                   synchronisation point
//
// The release/acquire pair on each ring publishes the PAYLOAD, not just the index: the writer's slot
// writes happen-before its store(release), and the reader's load(acquire) makes them visible. Both
// capacities are POWERS OF TWO so the index is a single mask, never a modulo.
// ============================================================================================

#include <aero/audio/clip.hpp>
#include <aero/audio/mixer.hpp>
#include <aero/audio/spatial.hpp>
#include <aero/core/guid.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine::audio {

struct AudioSystemConfig {
    float masterVolume = 1.0F;  // room for future knobs; the SceneRendererConfig{} precedent
};

struct AudioStats {
    std::uint32_t activeVoices = 0;
    std::uint32_t peakVoices = 0;
    std::uint32_t clipCount = 0;
    std::uint64_t droppedCommands = 0;  // SetParams/SetListener/SetMasterVolume discarded under back-pressure
    std::uint64_t rejectedPlays = 0;    // no free slot, ring full, or an unknown clip
    std::uint64_t framesRendered = 0;
    std::uint64_t callbacksCompleted = 0;
};

class AudioSystem {
public:
    // new (std::nothrow), NOT std::make_unique: docs/04 forbids an exception across a public API
    // boundary and make_unique throws. Returns null on allocation failure.
    [[nodiscard]] static std::unique_ptr<AudioSystem> create(const AudioSystemConfig& config = {});

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;
    ~AudioSystem();

    // ---- clips. Registration is GAME-THREAD ONLY, and there is deliberately no retirement (D7). --
    // Takes ownership. Invalid past MAX_CLIPS, counted, with ONE latched WARN per system lifetime.
    // The Guid key is the clip's own sourceGuid(); registering a second clip under a Guid ALREADY
    // PRESENT replaces the mapping and leaves the first clip RESIDENT AND PLAYABLE through its
    // existing handle -- "replace" and "evict" are different things and only one of them is safe with
    // no retirement.
    [[nodiscard]] ClipHandle registerClip(AudioClip&& clip);
    [[nodiscard]] ClipHandle findClip(Guid guid) const noexcept;  // invalid for nil or absent
    [[nodiscard]] std::uint32_t clipCount() const noexcept;

    // ---- voices. All game-thread; all noexcept; EVERY STALE HANDLE IS AN INERT NO-OP. ------------
    // play() ALLOCATES THE SLOT FIRST, THEN PUSHES, and if the push fails it RETURNS THE SLOT before
    // returning an invalid handle. Getting that backwards leaks one voice per failed play until the
    // system is destroyed, and THE LEAK IS INVISIBLE because activeVoices never rises: no counter
    // moves, nothing logs, playback simply degrades until nothing plays. SY7 is the only thing in the
    // tree that can see it.
    [[nodiscard]] VoiceHandle play(ClipHandle clip, const VoiceParams& params) noexcept;
    void stop(VoiceHandle voice) noexcept;
    void stopAll() noexcept;
    void setVolume(VoiceHandle voice, float volume) noexcept;
    void setPitch(VoiceHandle voice, float pitch) noexcept;
    void setPose(VoiceHandle voice, Vec3 worldPosition) noexcept;
    void setParams(VoiceHandle voice, const VoiceParams& params) noexcept;
    [[nodiscard]] bool isPlaying(VoiceHandle voice) const noexcept;

    // ---- global ---------------------------------------------------------------------------------
    void setListener(const ListenerPose& listener) noexcept;
    void setMasterVolume(float volume) noexcept;

    // ---- per frame ------------------------------------------------------------------------------
    // Drains the retire ring and returns finished slots to the free list. MUST BE CALLED ONCE PER
    // FRAME; SceneAudio::update() calls it FIRST. Without it slots leak, play() eventually refuses,
    // and rejectedPlays climbs -- which is why that counter exists and is reported rather than
    // swallowed. SY10 states the contract as a test rather than leaving it as folklore.
    void service() noexcept;
    [[nodiscard]] AudioStats stats() const noexcept;

    // ---- the render entry point. PUBLIC ON PURPOSE (D18). ---------------------------------------
    // Device-independent: tests and the sample's --dump-pcm call this directly, so the bytes anyone
    // measures come out of EXACTLY the code path a speaker hears, never a parallel offline renderer
    // that could drift. It drains the command ring and then calls the mixer.
    void render(std::span<float> output, std::uint32_t channels, std::uint32_t sampleRate) noexcept;

    // The two-line adapter matching platform::AudioRenderFn. `user` is an AudioSystem*; a NULL user
    // fills silence rather than crashing, which is what makes a pUserData that never got set a silent
    // failure on a realtime thread.
    static void renderCallback(void* user, std::span<float> output, std::uint32_t channels,
                               std::uint32_t sampleRate) noexcept;

private:
    explicit AudioSystem(const AudioSystemConfig& config);

    // Capacity is a POWER OF TWO so the index is a mask, never a modulo.
    static constexpr std::uint32_t COMMAND_RING_CAPACITY = 1024;
    static_assert((COMMAND_RING_CAPACITY & (COMMAND_RING_CAPACITY - 1)) == 0, "capacity must be a power of two");

    // BACK-PRESSURE IS GRADED, AND THE RESERVE IS WHAT MAKES THE GRADING WORK. A dropped SetParams is
    // benign -- the voice keeps its previous parameters and the next frame corrects it. A DROPPED
    // STOP IS A SOUND THAT NEVER ENDS. So Start/Stop/StopAll are never droppable, and the reserve
    // exists to keep room for exactly those three.
    static constexpr std::uint32_t DROP_RESERVE = 64;

    struct SlotState {
        std::uint32_t generation = 0;  // 0 == never allocated; a live handle's generation is >= 1
        // The last VoiceParams this slot was told, kept GAME-SIDE. setVolume/setPitch/setPose are
        // one-field edits over a struct the mixer replaces WHOLE, so the game thread has to know what
        // the other fields currently are; reading them back off the audio thread would be a race.
        VoiceParams lastParams{};
        bool inUse = false;
    };

    struct ClipEntry {
        Guid guid;
        ClipHandle handle;
    };

    [[nodiscard]] bool push(const AudioCommand& command) noexcept;
    [[nodiscard]] bool handleIsLive(VoiceHandle voice) const noexcept;
    void drainCommands() noexcept;

    AudioMixer mixer;

    // Game-thread only: the owning vector (addresses are stable forever, because the clips live in
    // unique_ptrs and nothing is ever erased) and a SORTED VECTOR keyed by Guid -- never
    // std::unordered_map, for the AssetBindingTable reason: MSVC's node containers are not
    // nothrow-movable (the 3.1.2 R9 / C2607 rule), and a few hundred entries binary-search faster
    // than they hash.
    std::vector<std::unique_ptr<AudioClip>> clips;
    std::vector<ClipEntry> clipsByGuid;
    bool clipCapWarned = false;

    // Game-thread only: the slot free list and every slot's generation.
    std::array<SlotState, MAX_VOICES> slots{};
    std::array<std::uint32_t, MAX_VOICES> freeSlots{};
    std::uint32_t freeCount = 0;
    bool voiceCapWarned = false;

    std::array<AudioCommand, COMMAND_RING_CAPACITY> commandRing{};
    std::atomic<std::uint32_t> commandHead{0};  // audio thread writes
    std::atomic<std::uint32_t> commandTail{0};  // game thread writes

    std::atomic<std::uint64_t> droppedCommands{0};
    std::atomic<std::uint64_t> rejectedPlays{0};
    std::uint32_t peakVoices = 0;
};

}  // namespace engine::audio
