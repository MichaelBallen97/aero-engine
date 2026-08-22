// Aero Engine — the audio cook (task 3.7.1): interleaved s16 PCM in, .aerowave v1 bytes out. See
// audio_cook.hpp for the contract and docs/09-file-formats.md section 14 for the normative format.
// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. NO FLOATING POINT ARITHMETIC OF ANY KIND — the one
// float this file touches is stats.durationSeconds, assigned once from cookedAudioDurationSeconds
// and never written to a byte (INV-A1).
//
// The header's field offsets are NOT declared here. They live once, in cooked_audio.hpp's `detail`
// namespace, shared with the parser in cooked_audio.cpp — a second copy of those ten numbers is
// exactly the disguise a swapped-offset defect wears.
#include <aero/assets/audio_cook.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::assets {
namespace {

using detail::H_CHANNELS;
using detail::H_COOKER_VERSION;
using detail::H_FORMAT_VERSION;
using detail::H_FRAME_COUNT;
using detail::H_GUID_HI;
using detail::H_GUID_LO;
using detail::H_MAGIC;
using detail::H_RESERVED_FLAGS;
using detail::H_SAMPLE_DATA_OFFSET;
using detail::H_SAMPLE_RATE;
using detail::H_TOTAL_BYTES;

// A refusal carries a message and NOTHING ELSE: no bytes, no stats. That is the whole content of the
// "bytes is empty IFF Refused" invariant on the producing side, and it is one function so no refusal
// site can forget half of it.
[[nodiscard]] AudioCookResult refuse(std::string message) {
    AudioCookResult out;
    out.status = AudioCookStatus::Refused;
    out.message = std::move(message);
    return out;
}

}  // namespace

AudioCookResult cookAudio(const AudioCookInput& input) {
    // 1. the sample rate, against BOTH bounds, each naming the value and the bound it broke.
    if (input.sampleRate < MIN_COOKED_AUDIO_SAMPLE_RATE) {
        return refuse(std::format("the source's sample rate is {} Hz, below the minimum of {} Hz", input.sampleRate,
                                  MIN_COOKED_AUDIO_SAMPLE_RATE));
    }
    if (input.sampleRate > MAX_COOKED_AUDIO_SAMPLE_RATE) {
        return refuse(std::format("the source's sample rate is {} Hz, above the maximum of {} Hz", input.sampleRate,
                                  MAX_COOKED_AUDIO_SAMPLE_RATE));
    }

    // 2. the channel count. Zero first, so the division below is total by construction rather than by
    //    a reader's trust.
    if (input.channels == 0) {
        return refuse("the source declares zero channels");
    }
    if (input.channels > MAX_COOKED_AUDIO_CHANNELS) {
        return refuse(
            std::format("the source has {} channels, over the cap of {}", input.channels, MAX_COOKED_AUDIO_CHANNELS));
    }

    // 3. divisibility. A sample count that is not a whole number of frames is a caller that has lost
    //    track of its own interleaving, and truncating it would silently shift every channel after
    //    the ragged frame.
    const std::uint64_t sampleCount = input.samples.size();
    if (sampleCount % input.channels != 0) {
        return refuse(
            std::format("{} samples is not a whole number of {}-channel frames", sampleCount, input.channels));
    }

    // 4. frameCount is DERIVED, never a parameter (INV-A5): two numbers that must agree are one
    //    number. Taking it as an argument is a signature change, so the wrong shape is unspellable
    //    rather than merely tested.
    const std::uint64_t frameCount = sampleCount / input.channels;
    if (frameCount == 0) {
        return refuse("the source decoded to zero frames");
    }
    if (frameCount > MAX_COOKED_AUDIO_FRAMES) {
        return refuse(std::format("the source has {} frames, over the cap of {}", frameCount, MAX_COOKED_AUDIO_FRAMES));
    }
    if (sampleCount > MAX_COOKED_AUDIO_SAMPLES) {
        return refuse(std::format("{} channels x {} frames is {} samples, over the cap of {}", input.channels,
                                  frameCount, sampleCount, MAX_COOKED_AUDIO_SAMPLES));
    }

    // 5. ONLY NOW is anything sized. Every cap above is checked BEFORE this line, so an over-long
    //    input costs no allocation at all -- VALIDATE BEFORE RESERVE.
    //
    //    std::vector<std::byte>(total) VALUE-INITIALIZES, so every byte is zero before any write. No
    //    uninitialised byte can reach a file even if a future field is added and its write forgotten,
    //    which is a class of defect no test written today can have an arm for.
    const std::size_t totalBytes =
        COOKED_AUDIO_HEADER_BYTES + static_cast<std::size_t>(COOKED_AUDIO_SAMPLE_BYTES * sampleCount);
    std::vector<std::byte> out(totalBytes);
    const std::span<std::byte> bytes(out);

    for (std::size_t i = 0; i < COOKED_AUDIO_MAGIC.size(); ++i) {
        bytes[H_MAGIC + i] = static_cast<std::byte>(COOKED_AUDIO_MAGIC[i]);
    }
    putU32(bytes, H_FORMAT_VERSION, COOKED_AUDIO_FORMAT_VERSION);
    putU32(bytes, H_COOKER_VERSION, COOKED_AUDIO_COOKER_VERSION);
    putU64(bytes, H_GUID_HI, input.sourceGuid.hi);
    putU64(bytes, H_GUID_LO, input.sourceGuid.lo);
    putU32(bytes, H_SAMPLE_RATE, input.sampleRate);
    putU32(bytes, H_CHANNELS, input.channels);
    putU32(bytes, H_FRAME_COUNT, static_cast<std::uint32_t>(frameCount));
    putU32(bytes, H_RESERVED_FLAGS, 0);
    putU64(bytes, H_SAMPLE_DATA_OFFSET, COOKED_AUDIO_HEADER_BYTES);
    putU64(bytes, H_TOTAL_BYTES, totalBytes);

    // The sample region, in the caller's order and nothing else. Each s16 becomes two little-endian
    // bytes through putU16 -- the only place a sample becomes bytes in this subsystem. The cast is a
    // reinterpretation of the bit pattern, which is exactly what a PCM file stores, and putU16's own
    // static_assert is what makes its byte order a build failure rather than a test.
    for (std::size_t i = 0; i < input.samples.size(); ++i) {
        putU16(bytes, COOKED_AUDIO_HEADER_BYTES + (COOKED_AUDIO_SAMPLE_BYTES * i),
               static_cast<std::uint16_t>(input.samples[i]));
    }

    AudioCookResult result;
    result.bytes = std::move(out);
    result.stats.frameCount = static_cast<std::uint32_t>(frameCount);
    result.stats.sampleCount = sampleCount;
    result.stats.byteSize = totalBytes;
    result.stats.durationSeconds = cookedAudioDurationSeconds(input.sampleRate, result.stats.frameCount);
    return result;
}

}  // namespace engine::assets
