// Aero Engine — the mixer's implementation (task 3.7.2). EVERY FUNCTION BELOW EXCEPT publishClip AND
// popRetired RUNS ON THE AUDIO THREAD, and every one of them allocates nothing, locks nothing, logs
// nothing and throws nothing. There is no AERO_LOG_* in this file and there must never be one:
// spdlog allocates and formats.

#include <aero/audio/mixer.hpp>
#include <aero/core/profiler.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::audio {
namespace {

// s16 -> float. Dividing by 32768 makes -32768 map to EXACTLY -1.0 and 32767 to 0.999969...; the
// asymmetry is inherent to two's complement and is NOT corrected, because dividing by 32767 instead
// would let a full-scale negative sample reach -1.0000305 and hit the output clamp.
constexpr float S16_TO_FLOAT = 1.0F / 32768.0F;

// 32.32 FIXED POINT: the value of exactly one frame. A `float` cursor is a REACHABLE DEFECT rather
// than a style preference (D15): a float has a 24-bit mantissa, so 2^24 == 16 777 216, while
// MAX_COOKED_AUDIO_FRAMES is 28 800 000. A float frame cursor therefore loses sub-sample precision
// INSIDE THE FORMAT'S OWN LEGAL RANGE -- not a theoretical concern, a file `aero_cooker audio` will
// happily produce. MX19 is the arm that justifies the type.
constexpr double FIXED_POINT_SCALE = 4294967296.0;                  // 2^32, as a double
constexpr float FIXED_POINT_FRACTION_SCALE = 1.0F / 4294967296.0F;  // 2^-32, as a float
constexpr std::uint64_t FIXED_POINT_FRACTION_MASK = 0xFFFFFFFFULL;

// Computed ONCE PER BLOCK, so the per-sample cost is one u64 add.
//
// THE DOUBLE IS DELIBERATE AND IS WHY THIS IS BIT-EXACT ON EVERY LANE: the conversion is one divide,
// one multiply and one convert, all IEEE-754-exact operations with NO libm call, so identical inputs
// give identical increments everywhere. That is what licenses MX5's and SY18's exact assertions and
// validation row 5's shasum. Contrast render::sampleAnimation, which docs/09 section 13.7 excludes
// from the determinism contract by name precisely because it reaches sin/acos/sqrt.
//
// RANGE, DONE RATHER THAN ASSUMED: the worst legal increment is pitch 4 x (384000 / 8000) = 192, i.e.
// 192 * 2^32 ~= 8.2e11, and the worst cursor is 28 800 000 * 2^32 ~= 1.24e17. Both are comfortably
// inside u64's 1.8e19. MX18 asserts it rather than trusting this paragraph.
//
// THE EXPLICIT FINITENESS ARM BELOW IS LOAD-BEARING AND IS NOT A BARE std::clamp. libc++'s
// std::clamp(NaN, lo, hi) returns NaN -- measured by task 3.6.3 and recorded -- so a bare clamp here
// produces a NaN product, and CONVERTING A NaN TO std::uint64_t IS UNDEFINED BEHAVIOUR, which UBSan
// traps on both Debug lanes. AudioSystem sanitises pitch at the API boundary (D9's first defence),
// but applyCommand is PUBLIC and takes a raw VoiceParams, so this is the arm that makes the mixer
// total on its own terms. A non-finite pitch becomes the field's documented default, matching what
// setPitch does one layer up so the two can never disagree. MX20 is its witness.
[[nodiscard]] std::uint64_t frameIncrement(std::uint32_t clipRate, std::uint32_t outRate, float pitch) noexcept {
    const float safePitch = std::isfinite(pitch) ? std::clamp(pitch, MIN_PITCH, MAX_PITCH) : 1.0F;
    const auto ratio = static_cast<double>(clipRate) / static_cast<double>(outRate);
    return static_cast<std::uint64_t>(ratio * static_cast<double>(safePitch) * FIXED_POINT_SCALE);
}

// THE FINAL CLAMP, and it is deliberately NOT a bare std::clamp: libc++'s std::clamp(NaN, lo, hi)
// returns NaN -- measured by task 3.6.3 and recorded -- so a bare clamp would let a poisoned sample
// straight through. It also bounds a 64-voice pile-up, which is real: v0 has no limiter, so summed
// gains can exceed 1 even with every voice at volume <= 1. A soft-knee limiter is 6.4's; hard
// clamping is honest and is what "v0 mixer" means.
//
// THE AUDIO CONSEQUENCE IS WORSE THAN A WRONG POSE: a NaN or +-inf in an output buffer is a
// FULL-SCALE DISCONTINUITY -- an audible bang, and on some backends a sustained one.
[[nodiscard]] float finiteClamp(float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
}

// The NON-spatialized gain vector: `volume` on every output channel a straight-through mapping would
// reach. It is file-local and appears in no public header and no signature (D-6.4).
[[nodiscard]] ChannelGains passthroughGains(std::uint32_t clipChannels, std::uint32_t outputChannels,
                                            float volume) noexcept {
    ChannelGains gains{};
    const std::uint32_t limit = std::min(outputChannels, MAX_AUDIO_OUTPUT_CHANNELS);
    for (std::uint32_t ch = 0; ch < limit; ++ch) {
        gains.gain[ch] = volume;
    }
    static_cast<void>(clipChannels);  // the fan-out/downmix decision lives in mixVoice's read path
    return gains;
}

// The arithmetic MEAN over a clip's channels at one frame, in float.
[[nodiscard]] float monoSample(const AudioClip& clip, std::uint32_t frame, std::uint32_t chans) noexcept {
    float sum = 0.0F;
    for (std::uint32_t c = 0; c < chans; ++c) {
        sum += static_cast<float>(clip.frameSample(frame, c)) * S16_TO_FLOAT;
    }
    return sum / static_cast<float>(chans);
}

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
            voice.currentGain = {};  // every voice ramps UP from silence on its first block
            voice.active = true;
            voice.stopping = false;
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
            voice.stopping = true;  // D17: the ramp does the work; retirement is at the block's end
            return;
        }
        case AudioCommand::Kind::StopAll: {
            for (Voice& voice : voices) {
                if (voice.active) {
                    voice.stopping = true;
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
                //
                // THE HANDLE'S GENERATION IS CHECKED, NOT ONLY ITS INDEX. Handle{} is
                // {index 0, generation 0}, so an index-only gate plays whatever is published at index
                // 0 when handed a DEFAULT-CONSTRUCTED ClipHandle. Unreachable through
                // AudioSystem::play (which refuses an invalid handle before allocating a slot), but
                // AudioMixer is a PUBLIC class and applyCommand is its documented audio-thread entry
                // point, so the invariant has to hold on its own terms. Guarded HERE rather than in
                // the Start arm so all four unusable-clip cases -- a null handle, an index past the
                // published count, a published null pointer and an empty clip -- RETIRE the voice by
                // the SAME path, which is what MX17's contract says and what keeps the retire ring's
                // accounting uniform.
                const AudioClip* clip =
                    voice.clip.valid() && voice.clip.index < published ? clipTable[voice.clip.index] : nullptr;
                if (clip == nullptr || !clip->valid() || clip->frameCount() == 0) {
                    voice.active = false;
                    retire(slot, voice.generation);
                    continue;
                }
                if (!mixVoice(voice, *clip, output, channels, frames, sampleRate)) {
                    voice.active = false;
                    retire(slot, voice.generation);
                }
            }
        }
    }

    // STEP 5 OF THE PER-BLOCK SEQUENCE: the finiteness-guarded clamp, per output sample. INV-2 -- the
    // mixer never writes a non-finite float -- is complete here, with all three of D9's defences in
    // place: sanitising at the API boundary (AudioSystem), computeSpatialGains's single whole-input
    // predicate, and this.
    for (float& sample : output) {
        sample = finiteClamp(sample);
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
                          std::size_t frames, std::uint32_t sampleRate) noexcept {
    const std::uint32_t clipFrames = clip.frameCount();
    const std::uint32_t clipChannels = clip.channels();
    const std::uint64_t increment = frameIncrement(clip.sampleRate(), sampleRate, voice.params.pitch);
    const bool loop = voice.params.loop;
    const bool spatialize = voice.params.spatialize;
    const std::uint64_t clipSpan = static_cast<std::uint64_t>(clipFrames) << 32U;
    const std::uint32_t gainChannels = std::min(channels, MAX_AUDIO_OUTPUT_CHANNELS);

    // ---- the block's TARGET gain, computed ONCE per block --------------------------------------
    ChannelGains target{};
    if (!voice.stopping) {
        if (spatialize) {
            const SpatialParams source{.position = voice.params.position,
                                       .minDistance = voice.params.minDistance,
                                       .maxDistance = voice.params.maxDistance,
                                       .volume = voice.params.volume};
            target = computeSpatialGains(listener, source, channels);
        } else {
            target = passthroughGains(clipChannels, channels, voice.params.volume);
        }
        // listener.volume and masterVolume are MIX-WIDE scalars applied uniformly to spatialized and
        // non-spatialized voices alike. Folding either into computeSpatialGains would make
        // passthroughGains need its own copy -- two places for one number.
        const float mixScale = listener.volume * masterVolume;
        for (float& value : target.gain) {
            value *= mixScale;
        }
    }
    // A stopping voice's target is the default-constructed all-zero ChannelGains: D17's "stop REUSES
    // the ramp instead of adding a path", spelled as the absence of an else.

    bool alive = true;
    const auto frameCountF = static_cast<float>(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        std::uint64_t frameIndex = voice.cursor >> 32U;
        if (frameIndex >= clipFrames) {
            if (!loop) {
                // Past the end of a non-looping clip: contribute EXACTLY 0 for the rest of the block
                // and retire at the block's END, never mid-walk.
                alive = false;
                break;
            }
            // THE WRAP IS IN THE FRAME LOOP, NOT AFTER IT. A block routinely spans the end of a short
            // clip -- a 64-frame clip against a 512-frame block passes it eight times -- and a wrap
            // deferred to the block's end would leave every frame past the end reading silence and
            // then retiring the voice. ONE MODULO RATHER THAN A SUBTRACT LOOP: a huge increment
            // (pitch 4 on an 8 kHz clip at 384 kHz output) can pass the end several times in a single
            // step, and a loop that subtracts is UNBOUNDED WORK ON A REALTIME THREAD. The modulo runs
            // only on the frames that actually wrap -- once per clip period in every ordinary case.
            voice.cursor %= clipSpan;
            frameIndex = voice.cursor >> 32U;
        }
        const auto index = static_cast<std::uint32_t>(frameIndex);
        // THE SECOND INDEX HAS EXACTLY TWO RULES, and the loop one is the interesting half.
        //   looping: i + 1 WRAPS to 0 at the last frame, so a clip cut at a whole number of cycles
        //            loops with NO DISCONTINUITY AT ALL -- the property is witnessed by arithmetic
        //            (MX8) rather than by listening.
        //   not looping: i + 1 CLAMPS to frameCount - 1.
        const std::uint32_t nextIndex = index + 1 < clipFrames ? index + 1 : (loop ? 0U : clipFrames - 1U);
        const auto fracBits = static_cast<float>(voice.cursor & FIXED_POINT_FRACTION_MASK);
        const float frac = fracBits * FIXED_POINT_FRACTION_SCALE;

        // (f + 1) / frames, NEVER f / frames: with the latter the last frame never reaches the target
        // and the ramp is permanently one frame short, which COMPOUNDS across blocks. Exact at both
        // ends -- t(frames - 1) is exactly 1.
        const float t = static_cast<float>(f + 1) / frameCountF;

        const std::size_t base = f * channels;
        if (spatialize || clipChannels != channels) {
            // A SOURCE HAS ONE POSITION, AND TWO CHANNELS AT ONE POSITION IS A CONTRADICTION (D14):
            // a spatialized voice is downmixed to the arithmetic MEAN before panning. Playing the
            // clip's L into the world's L would ignore the position entirely, and duplicating each
            // channel through the same pan is the mean with extra steps and twice the reads. The same
            // downmix serves an ordinary channel-count mismatch, fanned out instead of panned.
            const float a = monoSample(clip, index, clipChannels);
            const float b = monoSample(clip, nextIndex, clipChannels);
            const float sample = a + ((b - a) * frac);
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                const float current = ch < gainChannels ? voice.currentGain[ch] : 0.0F;
                const float goal = ch < gainChannels ? target.gain[ch] : 0.0F;
                output[base + ch] += sample * (current + ((goal - current) * t));
            }
        } else {
            // Straight through, per channel: a 2-channel clip's L and R stay L and R.
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                const float a = static_cast<float>(clip.frameSample(index, ch)) * S16_TO_FLOAT;
                const float b = static_cast<float>(clip.frameSample(nextIndex, ch)) * S16_TO_FLOAT;
                const float sample = a + ((b - a) * frac);
                const float current = ch < gainChannels ? voice.currentGain[ch] : 0.0F;
                const float goal = ch < gainChannels ? target.gain[ch] : 0.0F;
                output[base + ch] += sample * (current + ((goal - current) * t));
            }
        }
        voice.cursor += increment;
    }

    // COMMITTED AT THE END OF THE BLOCK, NEVER BEFORE THE FRAME LOOP. Committing first would make
    // every block flat at its target and delete the ramp entirely.
    voice.currentGain = target.gain;
    if (voice.stopping) {
        alive = false;  // the ramp reached zero within this block; retire at its end
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
