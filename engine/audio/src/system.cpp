// Aero Engine — AudioSystem's implementation (task 3.7.2). EVERYTHING HERE IS GAME-THREAD EXCEPT
// render(), drainCommands() and renderCallback(), which are the audio thread's and allocate nothing,
// lock nothing, log nothing and throw nothing.
//
// The two latched WARNs below are the ONLY logging in engine/audio, and both sit on the GAME thread.

#include <aero/audio/system.hpp>
#include <aero/core/log.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace engine::audio {
namespace {

// D9's FIRST defence, at the API boundary: a non-finite argument is REPLACED with the field's
// documented default rather than propagated. The mixer's own guards are the second and third; this
// one is what stops a poisoned value from ever reaching them.
[[nodiscard]] float sanitizeVolume(float volume) noexcept {
    return std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 1.0F;
}

[[nodiscard]] float sanitizePitch(float pitch) noexcept {
    return std::isfinite(pitch) ? std::clamp(pitch, MIN_PITCH, MAX_PITCH) : 1.0F;
}

[[nodiscard]] float sanitizeDistance(float distance) noexcept {
    // 0 rather than the field's default: a distance is a geometry input, and 0 is the only value
    // that is meaningful for both minDistance and maxDistance without inventing a scale.
    return std::isfinite(distance) ? distance : 0.0F;
}

// PER COMPONENT, never per vector: a Vec3 with one bad component is otherwise discarded whole.
[[nodiscard]] Vec3 sanitizePosition(Vec3 position) noexcept {
    return Vec3{std::isfinite(position.x) ? position.x : 0.0F, std::isfinite(position.y) ? position.y : 0.0F,
                std::isfinite(position.z) ? position.z : 0.0F};
}

[[nodiscard]] VoiceParams sanitizeParams(const VoiceParams& params) noexcept {
    VoiceParams clean = params;
    clean.position = sanitizePosition(params.position);
    clean.volume = sanitizeVolume(params.volume);
    clean.pitch = sanitizePitch(params.pitch);
    clean.minDistance = sanitizeDistance(params.minDistance);
    clean.maxDistance = sanitizeDistance(params.maxDistance);
    return clean;
}

[[nodiscard]] ListenerPose sanitizeListener(const ListenerPose& listener) noexcept {
    ListenerPose clean = listener;
    clean.position = sanitizePosition(listener.position);
    clean.right = sanitizePosition(listener.right);
    clean.up = sanitizePosition(listener.up);
    clean.forward = sanitizePosition(listener.forward);
    clean.volume = sanitizeVolume(listener.volume);
    return clean;
}

// Only these three may be dropped under back-pressure. A dropped SetParams is benign; A DROPPED STOP
// IS A SOUND THAT NEVER ENDS.
[[nodiscard]] bool commandIsDroppable(AudioCommand::Kind kind) noexcept {
    switch (kind) {
        case AudioCommand::Kind::SetParams:
        case AudioCommand::Kind::SetListener:
        case AudioCommand::Kind::SetMasterVolume:
            return true;
        case AudioCommand::Kind::Start:
        case AudioCommand::Kind::Stop:
        case AudioCommand::Kind::StopAll:
            return false;
    }
    return false;  // unreachable: the switch above has no default and covers every enumerator
}

}  // namespace

AudioSystem::AudioSystem(const AudioSystemConfig& config) {
    for (std::uint32_t slot = 0; slot < MAX_VOICES; ++slot) {
        freeSlots[slot] = MAX_VOICES - 1 - slot;  // popped from the back, so slot 0 is handed out first
    }
    freeCount = MAX_VOICES;
    // The master volume is a command like any other, so the mixer's copy and this one can never be
    // set by two different mechanisms.
    AudioCommand command;
    command.kind = AudioCommand::Kind::SetMasterVolume;
    command.masterVolume = sanitizeVolume(config.masterVolume);
    mixer.applyCommand(command);
}

AudioSystem::~AudioSystem() = default;

std::unique_ptr<AudioSystem> AudioSystem::create(const AudioSystemConfig& config) {
    // new (std::nothrow), never std::make_unique: docs/04 forbids an exception across a public API
    // boundary and make_unique throws.
    return std::unique_ptr<AudioSystem>{new (std::nothrow) AudioSystem{config}};
}

// ---- clips --------------------------------------------------------------------------------------

ClipHandle AudioSystem::registerClip(AudioClip&& clip) {
    if (clips.size() >= MAX_CLIPS) {
        if (!clipCapWarned) {
            clipCapWarned = true;  // LATCHED: once per system lifetime, never once per attempt
            AERO_LOG_WARN("audio: clip registry is full at {} clips; further registrations are refused",
                          static_cast<std::uint32_t>(MAX_CLIPS));
        }
        return {};
    }

    const Guid guid = clip.sourceGuid();
    const auto index = static_cast<std::uint32_t>(clips.size());
    clips.push_back(std::make_unique<AudioClip>(std::move(clip)));
    // generation is ALWAYS 1 in v0 -- there is no retirement (D7), and stating it is what makes
    // adding one later purely additive.
    const ClipHandle handle{.index = index, .generation = 1};
    if (!mixer.publishClip(index, clips.back().get())) {
        clips.pop_back();
        return {};
    }

    if (guid.valid()) {
        const auto position =
            std::lower_bound(clipsByGuid.begin(), clipsByGuid.end(), guid,
                             [](const ClipEntry& entry, const Guid& key) { return entry.guid < key; });
        if (position != clipsByGuid.end() && position->guid == guid) {
            // REPLACE the mapping; the older clip stays RESIDENT AND PLAYABLE through its existing
            // handle. Evicting it would dangle a pointer the audio thread may be reading.
            position->handle = handle;
        } else {
            clipsByGuid.insert(position, ClipEntry{.guid = guid, .handle = handle});
        }
    }
    return handle;
}

ClipHandle AudioSystem::findClip(Guid guid) const noexcept {
    if (!guid.valid()) {
        return {};  // nil is not a lookup key, it is the absence of one
    }
    const auto byGuid = [](const ClipEntry& entry, const Guid& key) { return entry.guid < key; };
    const auto position = std::lower_bound(clipsByGuid.begin(), clipsByGuid.end(), guid, byGuid);
    if (position == clipsByGuid.end() || position->guid != guid) {
        return {};
    }
    return position->handle;
}

std::uint32_t AudioSystem::clipCount() const noexcept { return static_cast<std::uint32_t>(clips.size()); }

// ---- the command ring ---------------------------------------------------------------------------

bool AudioSystem::push(const AudioCommand& command) noexcept {
    // Droppability is a property of the KIND, decided in ONE place above, never of the call site --
    // so making SetParams non-droppable or Stop droppable is a single-site change.
    const bool droppable = commandIsDroppable(command.kind);
    const std::uint32_t tail = commandTail.load(std::memory_order_relaxed);
    const std::uint32_t head = commandHead.load(std::memory_order_acquire);
    const std::uint32_t used = tail - head;
    const std::uint32_t free = COMMAND_RING_CAPACITY - used;

    if (droppable && free <= DROP_RESERVE) {
        droppedCommands.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (free == 0) {
        return false;  // the CALLER decides what a refused Start or Stop means
    }
    // The payload is written FIRST and the tail released SECOND: the release is what publishes the
    // slot's contents, not merely the index.
    commandRing[tail & (COMMAND_RING_CAPACITY - 1)] = command;
    commandTail.store(tail + 1, std::memory_order_release);
    return true;
}

void AudioSystem::drainCommands() noexcept {
    // AUDIO THREAD. Bounded by construction: at most COMMAND_RING_CAPACITY iterations, no allocation.
    const std::uint32_t tail = commandTail.load(std::memory_order_acquire);
    std::uint32_t head = commandHead.load(std::memory_order_relaxed);
    while (head != tail) {
        mixer.applyCommand(commandRing[head & (COMMAND_RING_CAPACITY - 1)]);
        ++head;
    }
    commandHead.store(head, std::memory_order_release);
}

// ---- voices -------------------------------------------------------------------------------------

bool AudioSystem::handleIsLive(VoiceHandle voice) const noexcept {
    if (!voice.valid() || voice.index >= MAX_VOICES) {
        return false;
    }
    const SlotState& slot = slots[voice.index];
    return slot.inUse && slot.generation == voice.generation;
}

VoiceHandle AudioSystem::play(ClipHandle clip, const VoiceParams& params) noexcept {
    if (!clip.valid() || clip.index >= clipCount()) {
        rejectedPlays.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    if (freeCount == 0) {
        if (!voiceCapWarned) {
            voiceCapWarned = true;  // LATCHED: once per system lifetime
            AERO_LOG_WARN("audio: all {} voices are in use; further play() calls are refused until one frees",
                          static_cast<std::uint32_t>(MAX_VOICES));
        }
        rejectedPlays.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    // ALLOCATE FIRST, THEN PUSH. The order is the whole content of the rule.
    --freeCount;
    const std::uint32_t index = freeSlots[freeCount];
    SlotState& slot = slots[index];
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;  // generation 0 is the reserved null sentinel and must never be minted
    }
    slot.inUse = true;

    AudioCommand command;
    command.kind = AudioCommand::Kind::Start;
    command.slot = index;
    command.generation = slot.generation;
    command.clip = clip;
    command.params = sanitizeParams(params);
    slot.lastParams = command.params;

    if (!push(command)) {
        // THE SLOT IS RETURNED BEFORE THE INVALID HANDLE IS. Getting this backwards leaks one voice
        // per failed play, and the leak is INVISIBLE because activeVoices never rises.
        slot.inUse = false;
        freeSlots[freeCount] = index;
        ++freeCount;
        rejectedPlays.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    const std::uint32_t inUse = MAX_VOICES - freeCount;
    peakVoices = std::max(peakVoices, inUse);
    return VoiceHandle{.index = index, .generation = slot.generation};
}

void AudioSystem::stop(VoiceHandle voice) noexcept {
    if (!handleIsLive(voice)) {
        return;  // a stale handle is an inert no-op on every one of the five entry points
    }
    AudioCommand command;
    command.kind = AudioCommand::Kind::Stop;
    command.slot = voice.index;
    command.generation = voice.generation;
    static_cast<void>(push(command));
}

void AudioSystem::stopAll() noexcept {
    AudioCommand command;
    command.kind = AudioCommand::Kind::StopAll;
    static_cast<void>(push(command));
}

void AudioSystem::setParams(VoiceHandle voice, const VoiceParams& params) noexcept {
    if (!handleIsLive(voice)) {
        return;
    }
    AudioCommand command;
    command.kind = AudioCommand::Kind::SetParams;
    command.slot = voice.index;
    command.generation = voice.generation;
    command.params = sanitizeParams(params);
    // Recorded even if the push is DROPPED under back-pressure: the game thread's view is what the
    // caller asked for, and the next frame's push corrects the mixer. Recording only on success would
    // make a dropped SetParams permanently invisible to a later one-field edit.
    slots[voice.index].lastParams = command.params;
    static_cast<void>(push(command));
}

void AudioSystem::setVolume(VoiceHandle voice, float volume) noexcept {
    if (!handleIsLive(voice)) {
        return;
    }
    VoiceParams params = slots[voice.index].lastParams;
    params.volume = sanitizeVolume(volume);
    setParams(voice, params);
}

void AudioSystem::setPitch(VoiceHandle voice, float pitch) noexcept {
    if (!handleIsLive(voice)) {
        return;
    }
    VoiceParams params = slots[voice.index].lastParams;
    params.pitch = sanitizePitch(pitch);
    setParams(voice, params);
}

void AudioSystem::setPose(VoiceHandle voice, Vec3 worldPosition) noexcept {
    if (!handleIsLive(voice)) {
        return;
    }
    VoiceParams params = slots[voice.index].lastParams;
    params.position = sanitizePosition(worldPosition);
    setParams(voice, params);
}

bool AudioSystem::isPlaying(VoiceHandle voice) const noexcept { return handleIsLive(voice); }

// ---- global -------------------------------------------------------------------------------------

void AudioSystem::setListener(const ListenerPose& listener) noexcept {
    AudioCommand command;
    command.kind = AudioCommand::Kind::SetListener;
    command.listener = sanitizeListener(listener);
    static_cast<void>(push(command));
}

void AudioSystem::setMasterVolume(float volume) noexcept {
    AudioCommand command;
    command.kind = AudioCommand::Kind::SetMasterVolume;
    command.masterVolume = sanitizeVolume(volume);
    static_cast<void>(push(command));
}

// ---- per frame ----------------------------------------------------------------------------------

void AudioSystem::service() noexcept {
    std::uint32_t slotIndex = 0;
    std::uint32_t generation = 0;
    while (mixer.popRetired(slotIndex, generation)) {
        if (slotIndex >= MAX_VOICES) {
            continue;
        }
        SlotState& slot = slots[slotIndex];
        if (!slot.inUse || slot.generation != generation) {
            continue;  // a retirement for a generation that has already been superseded
        }
        slot.inUse = false;
        freeSlots[freeCount] = slotIndex;
        ++freeCount;
    }
}

AudioStats AudioSystem::stats() const noexcept {
    return AudioStats{.activeVoices = mixer.activeVoices(),
                      .peakVoices = peakVoices,
                      .clipCount = clipCount(),
                      .droppedCommands = droppedCommands.load(std::memory_order_relaxed),
                      .rejectedPlays = rejectedPlays.load(std::memory_order_relaxed),
                      .framesRendered = mixer.framesRendered(),
                      .callbacksCompleted = mixer.callbacksCompleted()};
}

// ---- the render entry point ---------------------------------------------------------------------

void AudioSystem::render(std::span<float> output, std::uint32_t channels, std::uint32_t sampleRate) noexcept {
    drainCommands();
    mixer.render(output, channels, sampleRate);
}

void AudioSystem::renderCallback(void* user, std::span<float> output, std::uint32_t channels,
                                 std::uint32_t sampleRate) noexcept {
    auto* system = static_cast<AudioSystem*>(user);
    if (system == nullptr) {
        // The callback OWNS every element of an uninitialised buffer, even on its refusal path.
        std::fill(output.begin(), output.end(), 0.0F);
        return;
    }
    system->render(output, channels, sampleRate);
}

}  // namespace engine::audio
