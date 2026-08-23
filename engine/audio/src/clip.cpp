// Aero Engine — the runtime audio clip and its VFS loader (task 3.7.1). See clip.hpp for the
// contract, docs/09-file-formats.md section 14 for the normative .aerowave format, and
// engine/audio/CMakeLists.txt for why this target links no vcpkg package at all.
//
// NEVER THROWS. NEVER LOGS. NO DECODER. The only thing this file understands is a cooked artifact
// that has already passed assets::parseCookedAudio.
#include <aero/assets/cooked_audio.hpp>
#include <aero/audio/clip.hpp>
#include <aero/core/profiler.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::audio {

std::string_view audioClipLoadStatusLabel(AudioClipLoadStatus status) noexcept {
    switch (status) {
        case AudioClipLoadStatus::Ok:
            return "Ok";
        case AudioClipLoadStatus::NotFound:
            return "Not found";
        case AudioClipLoadStatus::Unreadable:
            return "Unreadable";
        case AudioClipLoadStatus::TooLarge:
            return "Too large";
        case AudioClipLoadStatus::ParseFailed:
            return "Parse failed";
    }
    return "Unknown";  // unreachable; the switch has no default so a new enumerator is a -Wswitch error
}

bool AudioClip::valid() const noexcept {
    // Every one of the four has to hold: a moved-from clip keeps its scalars but loses its storage,
    // and a scalar-only test would call it valid while sampleBytes() came back empty.
    return !storage.empty() && rate != 0 && chans != 0 && frames != 0;
}

std::uint64_t AudioClip::sampleCount() const noexcept { return static_cast<std::uint64_t>(chans) * frames; }

float AudioClip::durationSeconds() const noexcept {
    // THE one spelling, used rather than restated. cookedAudioDurationSeconds is total at rate 0, so
    // a default-constructed clip answers 0.0F through the same function a loaded one does.
    return assets::cookedAudioDurationSeconds(rate, frames);
}

std::span<const std::byte> AudioClip::sampleBytes() const noexcept {
    if (!valid()) {
        return {};
    }
    const std::uint64_t length = assets::COOKED_AUDIO_SAMPLE_BYTES * sampleCount();
    // Defence in depth over a buffer parseCookedAudio already validated: a SUBTRACTION against the
    // known-good size, never an addition that can wrap. Both bounds come from this object's members.
    if (storage.size() < assets::COOKED_AUDIO_HEADER_BYTES ||
        storage.size() - assets::COOKED_AUDIO_HEADER_BYTES < length) {
        return {};
    }
    return std::span<const std::byte>{storage}.subspan(assets::COOKED_AUDIO_HEADER_BYTES,
                                                       static_cast<std::size_t>(length));
}

std::int16_t AudioClip::frameSample(std::uint32_t frame, std::uint32_t channel) const noexcept {
    // TOTAL in both dimensions, and the two bounds are checked SEPARATELY rather than folded into one
    // index comparison: a channel past `chans` with a frame in range would otherwise land on the next
    // frame's data and read a real sample, which is worse than silence because it is plausible.
    if (frame >= frames || channel >= chans) {
        return 0;
    }
    const std::uint64_t index = (static_cast<std::uint64_t>(frame) * chans) + channel;
    return assets::audioSample(sampleBytes(), index);
}

AudioClipLoadResult loadAudioClip(const VirtualFileSystem& vfs, std::string_view virtualPath) {
    AERO_PROFILE_ZONE_NAMED("audio::loadAudioClip");

    AudioClipLoadResult out;

    // 1. exists(). NOT redundant with readFile(): vfs.hpp:87-89 records that a nullopt read means
    //    "absent OR unreadable" and does not distinguish them, so this call is the only thing that
    //    separates NotFound from Unreadable.
    if (!vfs.exists(virtualPath)) {
        out.status = AudioClipLoadStatus::NotFound;
        out.message = std::format("no file at '{}'", virtualPath);
        return out;
    }

    // 2. fileSize() BEFORE readFile(), so a hostile or corrupt multi-gigabyte file costs a stat
    //    rather than a multi-gigabyte allocation. The cap is the container's own, never a second
    //    number: assets::MAX_COOKED_AUDIO_BYTES is derived from the header size and the sample cap.
    const std::optional<std::uint64_t> size = vfs.fileSize(virtualPath);
    if (!size.has_value()) {
        out.status = AudioClipLoadStatus::Unreadable;
        out.message = std::format("'{}' exists but its size could not be read", virtualPath);
        return out;
    }
    if (*size > assets::MAX_COOKED_AUDIO_BYTES) {
        out.status = AudioClipLoadStatus::TooLarge;
        out.message =
            std::format("'{}' is {} bytes, over the cap of {}", virtualPath, *size, assets::MAX_COOKED_AUDIO_BYTES);
        return out;
    }

    // 3. the read.
    std::optional<ByteBuffer> bytes = vfs.readFile(virtualPath);
    if (!bytes.has_value()) {
        out.status = AudioClipLoadStatus::Unreadable;
        out.message = std::format("'{}' exists but could not be read", virtualPath);
        return out;
    }

    // 4. the parse. Its message carries the offending value and its bound already, so this layer
    //    prefixes the LABEL and adds nothing else -- two layers restating one failure in two
    //    wordings is how the two drift apart.
    {
        const assets::CookedAudioParseResult parsed = assets::parseCookedAudio(*bytes);
        if (parsed.status != assets::CookedAudioStatus::Ok) {
            out.status = AudioClipLoadStatus::ParseFailed;
            out.message = std::format("{}: {}", assets::cookedAudioStatusLabel(parsed.status), parsed.message);
            return out;
        }
        // The four scalars are copied out HERE, inside the parse result's own scope, so that
        // `parsed.audio.bytes` -- a span into the very buffer about to be moved from -- is dead by
        // the time the move below happens. That ordering is the whole reason AudioClip holds no view.
        out.clip.rate = parsed.audio.sampleRate;
        out.clip.chans = parsed.audio.channels;
        out.clip.frames = parsed.audio.frameCount;
        out.clip.guid = parsed.audio.sourceGuid;
    }

    // 5. the move. The clip owns the file buffer whole -- header included -- and sampleBytes()
    //    subspans it from the fixed offset. Nothing here copies the sample region (INV-A2).
    out.clip.storage = std::move(*bytes);
    return out;
}

}  // namespace engine::audio
