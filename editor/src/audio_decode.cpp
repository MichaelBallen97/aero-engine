// Aero Engine — the compressed-audio bytes -> interleaved s16 adapter (task 3.7.1). See
// audio_decode.hpp for the contract. PURE: no disk, no ImGui, no SDL, no <filesystem>, no logging.
#include <aero/editor/audio_decode.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ---- stb_vorbis hygiene, ABOVE the include, all in this one TU ---------------------------------
// The ONLY <stb_vorbis.c> TU anywhere in this tree. stb_vorbis.c is header AND implementation in one
// file (its own #ifndef STB_VORBIS_HEADER_ONLY at :420 closes at :5541), so a second including TU is
// a duplicate-symbol link error, exactly like a second MINIAUDIO_IMPLEMENTATION.
#define STB_VORBIS_NO_STDIO  // removes stb_vorbis_open_filename/open_file/open_file_section and
                             // <stdio.h> from the TU entirely, so "all disk access goes through the
                             // editor's own primitives" cannot be broken here by accident.
// NOTE, MEASURED AGAINST THE PINNED SOURCE, and it corrects the spec: STB_VORBIS_NO_STDIO does NOT
// remove stb_vorbis_decode_memory. That function sits under :260's single
// !defined(STB_VORBIS_NO_INTEGER_CONVERSION); it is :258's stb_vorbis_decode_filename that carries
// the two-condition guard. The function is AVAILABLE and is deliberately NOT CALLED: it mallocs the
// whole decode with no cap the caller can impose, which is exactly the posture decodeImageRgba8
// established one asset class over. The prohibition is a GREP (AD18), not a #if.
// NOLINTBEGIN -- vendored, third-party code this project neither owns nor may patch.
#include <stb_vorbis.c>
// NOLINTEND
// DECLARATIONS only. The ONE implementation is engine/platform/src/miniaudio_impl.c, whose archive
// already reaches this target through aero_editor_core's PUBLIC aero::platform link; a second
// MINIAUDIO_IMPLEMENTATION anywhere in this tree is a duplicate-symbol link error.
// NOLINTBEGIN -- vendored, third-party code this project neither owns nor may patch.
#include <miniaudio.h>
// NOLINTEND

namespace engine::editor {

namespace {

// 4096 frames. At the 8-channel cap that is 32 768 shorts -- three orders of magnitude below INT_MAX,
// which is the type stb_vorbis' num_shorts parameter uses -- and it bounds the per-iteration scratch
// at 64 KiB.
constexpr std::uint32_t CHUNK_FRAMES = 4096;

[[nodiscard]] DecodedAudio refuse(AudioSourceFormat format, std::string error) {
    DecodedAudio out;
    out.format = format;
    out.error = std::move(error);
    return out;
}

// Every read loop in this file shares these shapes, so neither backend can hold half the rule.
//
// THE CAP IS CHECKED TWICE AND THE SECOND CHECK IS THE ONE THAT MATTERS. The pre-allocation check
// below trusts the source's own length query; this one trusts nothing. A length query is an answer
// derived from the file's own claims, and some of those claims are taken unclamped -- dr_wav uses a
// Wave64 `fact` chunk's 8-byte sample count verbatim (miniaudio.h:81684-81685) while the file-size
// clamp at :81646-81652 applies to the data chunk instead, so a crafted W64 can declare a length its
// data neither supports nor is bounded by. The loop therefore stops at the cap regardless of what the
// header promised. A cap checked only against a self-reported length is not a cap.
[[nodiscard]] bool wouldExceedFrameCap(std::uint64_t decodedFrames, std::uint64_t incomingFrames,
                                       std::uint32_t maxFrames) noexcept {
    return decodedFrames + incomingFrames > maxFrames;
}

// THE PRODUCT, WHICH IS THE ONLY ONE OF THE THREE BOUNDS THAT BOUNDS BYTES. Saturating rather than
// wrapping: `frames` comes from a length query that a hostile file controls, and a wrapped product
// would report a SMALL number for a huge claim, which is the one arithmetic mistake that turns a cap
// into a hole. `channels` is non-zero at every call site (both backends refuse zero first), so the
// division is total.
[[nodiscard]] std::uint64_t sampleCountOrSaturate(std::uint64_t frames, std::uint32_t channels) noexcept {
    const auto wide = static_cast<std::uint64_t>(channels);
    if (wide == 0 || frames > std::numeric_limits<std::uint64_t>::max() / wide) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return frames * wide;
}

[[nodiscard]] bool wouldExceedSampleCap(std::uint64_t frames, std::uint32_t channels,
                                        std::uint64_t maxSamples) noexcept {
    return sampleCountOrSaturate(frames, channels) > maxSamples;
}

// A refusal naming the bound, for the in-loop check, which cannot know the source's true length.
[[nodiscard]] std::string frameCapMessage(std::uint32_t maxFrames) {
    return "the source decodes to more than " + std::to_string(maxFrames) + " frames, the cap this build reads";
}

[[nodiscard]] std::string sampleCapMessage(std::uint32_t channels, std::uint64_t maxSamples) {
    return "the source decodes to more than " + std::to_string(maxSamples) + " samples at " + std::to_string(channels) +
           " channels, the cap this build reads";
}

// A refusal naming BOTH the source's own count and the bound, for the pre-allocation check, which is
// the only place the true length is knowable.
[[nodiscard]] std::string declaredFrameCapMessage(std::uint64_t declaredFrames, std::uint32_t maxFrames) {
    return "the source declares " + std::to_string(declaredFrames) + " frames, over the cap of " +
           std::to_string(maxFrames);
}

// The PRODUCT form of the same, worded so a reader can tell which of the two pre-allocation checks
// fired: this one names the multiplication, cookAudio's own sample-cap refusal's shape.
[[nodiscard]] std::string declaredSampleCapMessage(std::uint64_t declaredFrames, std::uint32_t channels,
                                                   std::uint64_t maxSamples) {
    return "the source declares " + std::to_string(channels) + " channels x " + std::to_string(declaredFrames) +
           " frames, which is " + std::to_string(sampleCountOrSaturate(declaredFrames, channels)) +
           " samples, over the cap of " + std::to_string(maxSamples);
}

[[nodiscard]] std::string channelCapMessage(std::uint32_t channels, std::uint32_t maxChannels) {
    return "the source has " + std::to_string(channels) + " channels, over the cap of " + std::to_string(maxChannels);
}

// A scope-owning ma_decoder. The alternative -- a naked ma_decoder_uninit beside each of the eight
// early returns below -- is how one of them gets forgotten, which is a leak ASan's LeakSanitizer sees
// only on the Linux Debug lane. decodeImageRgba8's unique_ptr guard is the same idea; ma_decoder is a
// value rather than a pointer, so it takes a small struct instead.
class MaDecoderGuard {
public:
    MaDecoderGuard() = default;
    MaDecoderGuard(const MaDecoderGuard&) = delete;
    MaDecoderGuard& operator=(const MaDecoderGuard&) = delete;
    MaDecoderGuard(MaDecoderGuard&&) = delete;
    MaDecoderGuard& operator=(MaDecoderGuard&&) = delete;
    ~MaDecoderGuard() {
        if (live) {
            ma_decoder_uninit(&decoder);
        }
    }

    [[nodiscard]] ma_decoder* get() noexcept { return &decoder; }
    void markLive() noexcept { live = true; }

private:
    ma_decoder decoder{};
    bool live = false;
};

// wav / flac / mp3. ONE code path for all three: the container is detected by miniaudio from the
// bytes, and the file name only chose THIS path rather than the ogg one.
[[nodiscard]] DecodedAudio decodeWithMiniaudio(AudioSourceFormat format, std::span<const std::byte> fileBytes,
                                               std::uint32_t maxFrames, std::uint32_t maxChannels,
                                               std::uint64_t maxSamples) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
    // channels = 0 and sampleRate = 0 are miniaudio's DOCUMENTED "use the stream's internal value"
    // sentinels. Any other value silently engages ma_data_converter and makes this decode resample or
    // remix, which the cook forbids -- and it would break the external anchor, since a resampled
    // stream cannot be byte-identical to ffmpeg's decode of the same file.
    config.ditherMode = ma_dither_mode_none;
    // EXPLICIT, even though ma_decoder_config_init opens with MA_ZERO_OBJECT so this is already 0.
    // DITHER IS RANDOMISED BY CONSTRUCTION: a dithered f32 -> s16 conversion produces different bytes
    // on CONSECUTIVE RUNS OF THE SAME BINARY ON THE SAME MACHINE, which would redden the cook's own
    // determinism arm intermittently and send whoever hits it looking for a cross-lane problem that
    // does not exist. Relying on a zero-initialised default for a field whose wrong value is
    // NON-REPRODUCIBLE is exactly the kind of silence this codebase converts into an assertion.

    MaDecoderGuard guard;
    const ma_result initResult = ma_decoder_init_memory(fileBytes.data(), fileBytes.size(), &config, guard.get());
    if (initResult != MA_SUCCESS) {
        return refuse(format, "no decoder in this build could open the file (miniaudio result " +
                                  std::to_string(static_cast<int>(initResult)) + ")");
    }
    guard.markLive();

    const std::uint32_t sampleRate = guard.get()->outputSampleRate;
    const std::uint32_t channels = guard.get()->outputChannels;
    if (channels == 0) {
        return refuse(format, "the source declares zero channels");
    }
    if (channels > maxChannels) {
        return refuse(format, channelCapMessage(channels, maxChannels));
    }

    DecodedAudio out;
    out.format = format;
    out.sampleRate = sampleRate;
    out.channels = channels;

    // The length query, and its answer is a CLAIM rather than a fact. A failure or a zero means
    // "unknown" -- never "empty" -- so nothing is reserved and the loop decides.
    //
    // THIS QUERY IS NOT FREE FOR MP3, AND THE COST IS STATED RATHER THAN DISCOVERED. An earlier
    // version of this comment said an mp3 with no Xing header reports 0 frames. That is FALSE for the
    // pinned miniaudio: ma_dr_mp3_get_pcm_frame_count (:95477-95500) returns a cached count only when
    // a Xing/Info tag set detectedMP3FrameCount (:94920, :94973-94974); with no such tag it falls
    // through to ma_dr_mp3_get_mp3_and_pcm_frame_count (:95440-95475), which DECODES THE ENTIRE
    // STREAM to count it and seeks back. For a seekable memory stream it therefore never returns 0 --
    // it returns the TRUE count.
    //
    // So a Xing-less mp3 is decoded TWICE: once to count, once for real. The counting pass allocates
    // no PCM (it passes a null output buffer) and its cost is CPU time proportional to the file, and
    // the file is already bounded by the caller's own read cap -- MAX_AUDIO_FILE_BYTES, 256 MiB at
    // the cooker's call site -- so it is bounded, but it is NOT bounded by maxFrames: it happens
    // before this function can refuse anything. Accepted, and the alternative (skipping the query for
    // mp3) would trade a bounded second pass for an unreserved vector growing one chunk at a time.
    //
    // NOTHING IS RESERVED UNTIL BOTH CLAIMS HAVE BEEN BOUNDED, and the ORDER is load-bearing: the
    // frame cap runs first, so `declaredFrames <= maxFrames` holds by the time the product is formed
    // and the saturating multiply below cannot be reached with the production caps. It saturates
    // anyway, because this function's bounds are parameters and a future caller's are not this
    // function's to assume.
    ma_uint64 declaredFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(guard.get(), &declaredFrames) == MA_SUCCESS && declaredFrames > 0) {
        if (declaredFrames > maxFrames) {
            return refuse(format, declaredFrameCapMessage(declaredFrames, maxFrames));
        }
        if (wouldExceedSampleCap(declaredFrames, channels, maxSamples)) {
            return refuse(format, declaredSampleCapMessage(declaredFrames, channels, maxSamples));
        }
        out.samples.reserve(static_cast<std::size_t>(declaredFrames) * channels);
    }

    std::vector<std::int16_t> scratch(static_cast<std::size_t>(CHUNK_FRAMES) * channels);
    std::uint64_t decodedFrames = 0;
    while (true) {
        ma_uint64 framesRead = 0;
        const ma_result readResult = ma_decoder_read_pcm_frames(guard.get(), scratch.data(), CHUNK_FRAMES, &framesRead);
        if (framesRead == 0) {
            break;  // MA_AT_END, or a mid-stream failure with nothing further to give
        }
        if (wouldExceedFrameCap(decodedFrames, framesRead, maxFrames)) {
            return refuse(format, frameCapMessage(maxFrames));
        }
        if (wouldExceedSampleCap(decodedFrames + framesRead, channels, maxSamples)) {
            return refuse(format, sampleCapMessage(channels, maxSamples));
        }
        const std::size_t produced = static_cast<std::size_t>(framesRead) * channels;
        out.samples.insert(out.samples.end(), scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(produced));
        decodedFrames += framesRead;
        if (readResult != MA_SUCCESS) {
            break;  // a short final read; keep what it gave and stop
        }
    }

    if (decodedFrames == 0) {
        return refuse(format, "the source decoded to zero frames");
    }
    return out;
}

// ogg -- stb_vorbis, the PULL API. stb_vorbis_decode_memory is available (see the macro block above)
// and is deliberately never called: it mallocs the whole decode with no cap this function could
// impose.
[[nodiscard]] DecodedAudio decodeWithStbVorbis(std::span<const std::byte> fileBytes, std::uint32_t maxFrames,
                                               std::uint32_t maxChannels, std::uint64_t maxSamples) {
    constexpr AudioSourceFormat FORMAT = AudioSourceFormat::Ogg;

    // The narrowing to stb's own `int` length must be provably safe from THIS function alone, not
    // from a constant in another header -- the decodeImageRgba8:112-117 rule verbatim. Unreachable
    // behind the 256 MiB read cap; present anyway.
    if (fileBytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return refuse(FORMAT, "the file is larger than this decoder can address");
    }
    const int length = static_cast<int>(fileBytes.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- texture_cook_source.cpp:119's idiom
    const auto* const raw = reinterpret_cast<const unsigned char*>(fileBytes.data());

    int openError = 0;
    stb_vorbis* const rawHandle = stb_vorbis_open_memory(raw, length, &openError, nullptr);
    if (rawHandle == nullptr) {
        return refuse(FORMAT,
                      "the Ogg Vorbis stream could not be opened (stb_vorbis error " + std::to_string(openError) + ")");
    }
    // OWNED FROM HERE. Every path below returns through this guard, which is what makes the early
    // returns safe -- a naked stb_vorbis_close beside four returns is how one of them gets forgotten.
    const std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> handle{rawHandle, &stb_vorbis_close};

    const stb_vorbis_info info = stb_vorbis_get_info(handle.get());
    if (info.channels <= 0) {
        return refuse(FORMAT, "the stream declares a non-positive channel count");
    }
    const auto channels = static_cast<std::uint32_t>(info.channels);
    if (channels > maxChannels) {
        return refuse(FORMAT, channelCapMessage(channels, maxChannels));
    }

    DecodedAudio out;
    out.format = FORMAT;
    out.sampleRate = info.sample_rate;
    out.channels = channels;

    // A CLAIM, not a fact: a truncated or crafted stream can report a granule position the pages that
    // follow do not support. 0 means "unknown" and reserves nothing. Both bounds are applied before
    // the reserve, frame cap first, for the ordering reason the miniaudio arm's own note gives.
    const std::uint64_t declaredFrames = stb_vorbis_stream_length_in_samples(handle.get());
    if (declaredFrames > 0) {
        if (declaredFrames > maxFrames) {
            return refuse(FORMAT, declaredFrameCapMessage(declaredFrames, maxFrames));
        }
        if (wouldExceedSampleCap(declaredFrames, channels, maxSamples)) {
            return refuse(FORMAT, declaredSampleCapMessage(declaredFrames, channels, maxSamples));
        }
        out.samples.reserve(static_cast<std::size_t>(declaredFrames) * channels);
    }

    std::vector<std::int16_t> scratch(static_cast<std::size_t>(CHUNK_FRAMES) * channels);
    // Explicit, and bounded by construction: CHUNK_FRAMES * MAX channels is 32 768, three orders of
    // magnitude below INT_MAX, and `channels` is already inside its own cap here.
    const int chunkShorts = static_cast<int>(scratch.size());
    std::uint64_t decodedFrames = 0;
    while (true) {
        const int framesRead =
            stb_vorbis_get_samples_short_interleaved(handle.get(), info.channels, scratch.data(), chunkShorts);
        if (framesRead <= 0) {
            break;  // 0 is end of stream; a negative value is not documented and is treated as one
        }
        const auto produced = static_cast<std::uint64_t>(framesRead);
        if (wouldExceedFrameCap(decodedFrames, produced, maxFrames)) {
            return refuse(FORMAT, frameCapMessage(maxFrames));
        }
        if (wouldExceedSampleCap(decodedFrames + produced, channels, maxSamples)) {
            return refuse(FORMAT, sampleCapMessage(channels, maxSamples));
        }
        out.samples.insert(out.samples.end(), scratch.begin(),
                           scratch.begin() + static_cast<std::ptrdiff_t>(produced * channels));
        decodedFrames += produced;
    }

    if (decodedFrames == 0) {
        return refuse(FORMAT, "the source decoded to zero frames");
    }
    return out;
}

}  // namespace

DecodedAudio decodeAudioFile(std::string_view fileName, std::span<const std::byte> fileBytes, std::uint32_t maxFrames,
                             std::uint32_t maxChannels, std::uint64_t maxSamples) {
    // BACKEND SELECTION IS BY FILE NAME, NEVER BY CONTENT SNIFFING, and it is one switch so a fifth
    // format lands in exactly one place. No `default:`, so a new AudioSourceFormat is a -Wswitch
    // failure on the Linux lane rather than a silent refusal.
    const AudioSourceFormat format = audioSourceFormatForName(fileName);
    switch (format) {
        case AudioSourceFormat::Unknown:
            return refuse(AudioSourceFormat::Unknown,
                          "no audio decoder claims this file type (expected .wav, .flac, .mp3 or .ogg)");
        case AudioSourceFormat::Wav:
        case AudioSourceFormat::Flac:
        case AudioSourceFormat::Mp3:
            if (fileBytes.empty()) {
                return refuse(format, "the file is empty");
            }
            return decodeWithMiniaudio(format, fileBytes, maxFrames, maxChannels, maxSamples);
        case AudioSourceFormat::Ogg:
            if (fileBytes.empty()) {
                return refuse(format, "the file is empty");
            }
            return decodeWithStbVorbis(fileBytes, maxFrames, maxChannels, maxSamples);
    }
    return refuse(AudioSourceFormat::Unknown,
                  "no audio decoder claims this file type (expected .wav, .flac, .mp3 or .ogg)");
}

}  // namespace engine::editor
