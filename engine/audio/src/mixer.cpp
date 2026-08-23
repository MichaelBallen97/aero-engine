// Aero Engine — the mixer's implementation (task 3.7.2). EVERY FUNCTION BELOW EXCEPT publishClip AND
// popRetired RUNS ON THE AUDIO THREAD, and every one of them allocates nothing, locks nothing, logs
// nothing and throws nothing. There is no AERO_LOG_* in this file and there must never be one:
// spdlog allocates and formats.

#include <aero/audio/mixer.hpp>
#include <aero/core/profiler.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::audio {
namespace {

// s16 -> float. Dividing by 32768 makes -32768 map to EXACTLY -1.0 and 32767 to 0.999969...; the
// asymmetry is inherent to two's complement and is NOT corrected, because dividing by 32767 instead
// would let a full-scale negative sample reach -1.0000305 and hit the output clamp.
constexpr float S16_TO_FLOAT = 1.0F / 32768.0F;

// 32.32 fixed point: the value of exactly one frame.
constexpr std::uint64_t FIXED_POINT_ONE = 1ULL << 32U;

}  // namespace

bool AudioMixer::publishClip(std::uint32_t index, const AudioClip* clip) noexcept {
    const std::uint32_t published = clipCount.load(std::memory_order_relaxed);
    if (index >= MAX_CLIPS || index != published) {
        return false;  // out of range, or out of order -- publication is strictly append-only
    }
    // The pointer is written FIRST and the count released SECOND. The audio thread's acquire-load of
    // the count is what makes this write visible; it reads no pointer at or past the count it saw.
    clipTable[index] = clip;
    clipCount.store(index + 1, std::memory_order_release);
    return true;
}

bool AudioMixer::popRetired(std::uint32_t& slot, std::uint32_t& generation) noexcept {
    const std::uint32_t head = retireHead.load(std::memory_order_relaxed);
    const std::uint32_t tail = retireTail.load(std::memory_order_acquire);
    if (head == tail) {
        return false;
    }
    const RetiredVoice& entry = retireRing[head & (RETIRE_RING_CAPACITY - 1)];
    slot = entry.slot;
    generation = entry.generation;
    retireHead.store(head + 1, std::memory_order_release);
    return true;
}

void AudioMixer::retire(std::uint32_t slot, std::uint32_t generation) noexcept {
    const std::uint32_t tail = retireTail.load(std::memory_order_relaxed);
    const std::uint32_t head = retireHead.load(std::memory_order_acquire);
    if (tail - head >= RETIRE_RING_CAPACITY) {
        return;  // unreachable by mixer.hpp's capacity proof; a realtime thread has nothing better to do
    }
    retireRing[tail & (RETIRE_RING_CAPACITY - 1)] = RetiredVoice{.slot = slot, .generation = generation};
    retireTail.store(tail + 1, std::memory_order_release);
}

void AudioMixer::applyCommand(const AudioCommand& command) noexcept {
    // A switch with NO `default:` -- a future enumerator is a -Wswitch failure on the Linux lane
    // rather than a silent fallthrough.
    switch (command.kind) {
        case AudioCommand::Kind::Start: {
            if (command.slot >= MAX_VOICES) {
                return;
            }
            Voice& voice = voices[command.slot];
            voice.generation = command.generation;
            voice.clip = command.clip;
            voice.params = command.params;
            voice.cursor = 0;
            voice.active = true;
            return;
        }
        case AudioCommand::Kind::Stop: {
            if (command.slot >= MAX_VOICES) {
                return;
            }
            Voice& voice = voices[command.slot];
            if (!voice.active || voice.generation != command.generation) {
                return;  // a stale handle is an inert no-op, never a stop of somebody else's voice
            }
            voice.active = false;
            retire(command.slot, voice.generation);
            return;
        }
        case AudioCommand::Kind::StopAll: {
            for (std::uint32_t slot = 0; slot < MAX_VOICES; ++slot) {
                Voice& voice = voices[slot];
                if (voice.active) {
                    voice.active = false;
                    retire(slot, voice.generation);
                }
            }
            return;
        }
        case AudioCommand::Kind::SetParams: {
            if (command.slot >= MAX_VOICES) {
                return;
            }
            Voice& voice = voices[command.slot];
            if (!voice.active || voice.generation != command.generation) {
                return;
            }
            voice.params = command.params;
            return;
        }
        case AudioCommand::Kind::SetListener:
            listener = command.listener;
            return;
        case AudioCommand::Kind::SetMasterVolume:
            masterVolume = command.masterVolume;
            return;
    }
}

void AudioMixer::render(std::span<float> output, std::uint32_t channels, std::uint32_t sampleRate) noexcept {
    AERO_PROFILE_ZONE_NAMED("audio.mix");
    // Name the audio thread once so it is legible in a Tracy capture (dev builds only, cheap one-shot)
    // -- the audio_device.cpp idiom, reused rather than reinvented.
    static thread_local bool named = false;
    if (!named) {
        AERO_PROFILE_SET_THREAD_NAME("Audio");
        named = true;
    }

    // NOT AN OPTIMISATION AND NOT OPTIONAL: miniaudio hands the callback an UNINITIALISED buffer, so a
    // mixer that wrote only where voices are audible would play whatever was in that memory.
    std::fill(output.begin(), output.end(), 0.0F);

    std::uint64_t framesThisBlock = 0;
    // channels == 0 or sampleRate == 0 means NO DIVISION IS PERFORMED AT ALL. The buffer is already
    // silent and the counters below are still published.
    if (channels != 0 && sampleRate != 0) {
        const std::size_t frames = output.size() / channels;  // the remainder stays ZEROED, never read
        framesThisBlock = frames;
        if (frames != 0) {
            const std::uint32_t published = clipCount.load(std::memory_order_acquire);
            for (std::uint32_t slot = 0; slot < MAX_VOICES; ++slot) {
                Voice& voice = voices[slot];
                if (!voice.active) {
                    continue;
                }
                // The bounds check is belt AND braces: AudioClip::frameSample is total in both
                // dimensions, but this is the one path in the tree where an out-of-bounds read would
                // be HEARD as well as being undefined.
                const AudioClip* clip = voice.clip.index < published ? clipTable[voice.clip.index] : nullptr;
                if (clip == nullptr || !clip->valid() || clip->frameCount() == 0) {
                    voice.active = false;
                    retire(slot, voice.generation);
                    continue;
                }
                if (!mixVoice(voice, *clip, output, channels, frames)) {
                    voice.active = false;
                    retire(slot, voice.generation);
                }
            }
        }
    }

    // ONE publication point, reached on EVERY exit from this function including the channels == 0 one.
    // Task 3.6.1's own review found the mirror failure -- two Tracy plots skipped on an early return,
    // left holding the previous frame's values while the accessors correctly read 0 -- so this is that
    // fix applied BEFORE the defect rather than after. The active count is recomputed from the pool
    // rather than accumulated, so it is correct on the paths that mix nothing.
    std::uint32_t active = 0;
    for (const Voice& voice : voices) {
        if (voice.active) {
            ++active;
        }
    }
    activeCount.store(active, std::memory_order_relaxed);
    frameTotal.fetch_add(framesThisBlock, std::memory_order_relaxed);
    callbackTotal.fetch_add(1, std::memory_order_relaxed);
    // EXACTLY ONE CALL SITE, so a swapped plot name is a single-site seed (the render.drawn /
    // render.culled rule from 3.6.1).
    AERO_PROFILE_PLOT("audio.voices", static_cast<double>(active));
}

bool AudioMixer::mixVoice(Voice& voice, const AudioClip& clip, std::span<float> output, std::uint32_t channels,
                          std::size_t frames) noexcept {
    const std::uint32_t clipFrames = clip.frameCount();
    const std::uint32_t clipChannels = clip.channels();
    const float gain = voice.params.volume * masterVolume;
    // Unity increment at this commit: the fixed-point rate/pitch conversion and the interpolation
    // arrive with the resampler. The cursor is already 32.32 so that arrival changes only this line.
    const std::uint64_t increment = FIXED_POINT_ONE;

    bool alive = true;
    for (std::size_t f = 0; f < frames; ++f) {
        const std::uint64_t frameIndex = voice.cursor >> 32U;
        if (frameIndex >= clipFrames) {
            // Past the end of a non-looping clip: contribute EXACTLY 0 for the rest of the block and
            // retire at the block's end, never mid-walk.
            alive = false;
            break;
        }
        const auto index = static_cast<std::uint32_t>(frameIndex);
        const std::size_t base = f * channels;
        if (clipChannels == channels) {
            // Straight through, per channel: a 2-channel clip's L and R stay L and R.
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                const float sample = static_cast<float>(clip.frameSample(index, ch)) * S16_TO_FLOAT;
                output[base + ch] += sample * gain;
            }
        } else {
            // Channel-count mismatch: the arithmetic MEAN over the clip's channels, fanned out to
            // every output channel.
            float sum = 0.0F;
            for (std::uint32_t c = 0; c < clipChannels; ++c) {
                sum += static_cast<float>(clip.frameSample(index, c)) * S16_TO_FLOAT;
            }
            const float mono = sum / static_cast<float>(clipChannels);
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                output[base + ch] += mono * gain;
            }
        }
        voice.cursor += increment;
    }
    return alive;
}

// The three diagnostics accessors. RELAXED IN BOTH DIRECTIONS, deliberately: they are a readout,
// never a synchronisation point, and nothing's correctness anywhere depends on the value one returns.
std::uint32_t AudioMixer::activeVoices() const noexcept {
    // relaxed -- a readout, never a synchronisation point
    return activeCount.load(std::memory_order_relaxed);
}

std::uint64_t AudioMixer::framesRendered() const noexcept {
    // relaxed -- a readout, never a synchronisation point
    return frameTotal.load(std::memory_order_relaxed);
}

std::uint64_t AudioMixer::callbacksCompleted() const noexcept {
    // relaxed -- a readout, never a synchronisation point
    return callbackTotal.load(std::memory_order_relaxed);
}

}  // namespace engine::audio
