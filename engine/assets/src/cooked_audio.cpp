// Aero Engine — the cooked audio clip container v1: the status label, the sample accessor and the
// hostile-input parser (task 3.7.1). See cooked_audio.hpp for the contract and
// docs/09-file-formats.md section 14 for the normative format. NEVER THROWS. NEVER READS A FILE.
// NEVER LOGS. NO FLOATING POINT ANYWHERE IN THIS FILE.
//
// Nothing here reserves anything at all: the sample region is returned as a SPAN over the caller's
// own buffer and is never copied. That makes "nothing is reserved before the header's counts have
// passed their caps" trivially true, and it is said here rather than left implicit so nobody adds a
// copy later and quietly breaks it.
//
// The header's field offsets are NOT declared in this file. They live once, in cooked_audio.hpp's
// `detail` namespace, because audio_cook.cpp needs the same ten numbers and a second copy is exactly
// the disguise a swapped-offset defect wears.
#include <aero/assets/cooked_audio.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::assets {
namespace {

// The format's ten header offsets, reached by USING-DECLARATIONS rather than a using-directive: they
// are declared once in cooked_audio.hpp's `detail` namespace so this parser and audio_cook.cpp's
// writer share one copy, and naming each one here keeps the body readable without pulling in whatever
// else `detail` may hold later.
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

[[nodiscard]] CookedAudioParseResult refuse(CookedAudioStatus status, std::string message) {
    CookedAudioParseResult out;
    out.status = status;
    out.message = std::move(message);
    return out;
}

}  // namespace

std::string_view cookedAudioStatusLabel(CookedAudioStatus status) noexcept {
    switch (status) {
        case CookedAudioStatus::Ok:
            return "Ok";
        case CookedAudioStatus::TooSmall:
            return "Too small";
        case CookedAudioStatus::BadMagic:
            return "Bad magic";
        case CookedAudioStatus::UnsupportedVersion:
            return "Unsupported version";
        case CookedAudioStatus::ReservedNotZero:
            return "Reserved field not zero";
        case CookedAudioStatus::SizeMismatch:
            return "Size mismatch";
        case CookedAudioStatus::CapExceeded:
            return "Cap exceeded";
        case CookedAudioStatus::BadTable:
            return "Bad table";
        case CookedAudioStatus::BadRange:
            return "Bad range";
    }
    return "Unknown";  // unreachable; the switch has no default so a new enumerator is a -Wswitch error
}

std::span<const std::byte> audioSampleBytes(const CookedAudio& audio) noexcept {
    const std::uint64_t length =
        COOKED_AUDIO_SAMPLE_BYTES * static_cast<std::uint64_t>(audio.channels) * audio.frameCount;
    // Defence in depth over an already-validated parse: a SUBTRACTION against the known-good size,
    // never an addition that can wrap.
    if (audio.sampleDataOffset > audio.bytes.size() || audio.bytes.size() - audio.sampleDataOffset < length) {
        return {};
    }
    return audio.bytes.subspan(static_cast<std::size_t>(audio.sampleDataOffset), static_cast<std::size_t>(length));
}

CookedAudioParseResult parseCookedAudio(std::span<const std::byte> bytes) {
    // 1. shorter than the header.
    if (bytes.size() < COOKED_AUDIO_HEADER_BYTES) {
        return refuse(CookedAudioStatus::TooSmall,
                      std::format("the buffer is {} bytes, shorter than the {}-byte header", bytes.size(),
                                  COOKED_AUDIO_HEADER_BYTES));
    }

    // 2. magic. Compared BYTE BY BYTE over all eight bytes -- never a memcmp of a reinterpret_cast'd
    //    pointer, and never a prefix: the last byte is as load-bearing as the first.
    for (std::size_t i = 0; i < COOKED_AUDIO_MAGIC.size(); ++i) {
        if (bytes[H_MAGIC + i] != static_cast<std::byte>(COOKED_AUDIO_MAGIC[i])) {
            return refuse(CookedAudioStatus::BadMagic, "the buffer does not begin with AEROWAVE");
        }
    }

    // 3. version.
    const std::uint32_t formatVersion = getU32(bytes, H_FORMAT_VERSION);
    if (formatVersion != COOKED_AUDIO_FORMAT_VERSION) {
        return refuse(CookedAudioStatus::UnsupportedVersion,
                      std::format("cooked audio format version {} (this build reads version {})", formatVersion,
                                  COOKED_AUDIO_FORMAT_VERSION));
    }

    // 4. the header's reserved space. A REFUSAL, deliberately: occupying one of these is a
    //    formatVersion bump, so a non-zero value here is a file this build cannot claim to read.
    if (getU32(bytes, H_RESERVED_FLAGS) != 0) {
        return refuse(CookedAudioStatus::ReservedNotZero, "the header's reserved flags field is not zero");
    }

    // 5. the sample rate, against BOTH bounds. Named with its value AND its bound, the mesh_cook.cpp
    //    convention.
    const std::uint32_t sampleRate = getU32(bytes, H_SAMPLE_RATE);
    if (sampleRate < MIN_COOKED_AUDIO_SAMPLE_RATE) {
        return refuse(CookedAudioStatus::BadTable,
                      std::format("the header declares a sample rate of {} Hz, below the minimum of {} Hz", sampleRate,
                                  MIN_COOKED_AUDIO_SAMPLE_RATE));
    }
    if (sampleRate > MAX_COOKED_AUDIO_SAMPLE_RATE) {
        return refuse(CookedAudioStatus::BadTable,
                      std::format("the header declares a sample rate of {} Hz, above the maximum of {} Hz", sampleRate,
                                  MAX_COOKED_AUDIO_SAMPLE_RATE));
    }

    // 6/7. the two counts. Zero is BadTable -- that is what makes "a .aerowave is never empty" a
    //      parse requirement rather than a comment -- and over-cap is CapExceeded, so the two are
    //      distinguishable by STATUS and not only by message.
    const std::uint32_t channels = getU32(bytes, H_CHANNELS);
    if (channels == 0) {
        return refuse(CookedAudioStatus::BadTable, "the header declares zero channels");
    }
    if (channels > MAX_COOKED_AUDIO_CHANNELS) {
        return refuse(CookedAudioStatus::CapExceeded, std::format("the header declares {} channels, over the cap of {}",
                                                                  channels, MAX_COOKED_AUDIO_CHANNELS));
    }
    const std::uint32_t frameCount = getU32(bytes, H_FRAME_COUNT);
    if (frameCount == 0) {
        return refuse(CookedAudioStatus::BadTable, "the header declares zero frames");
    }
    if (frameCount > MAX_COOKED_AUDIO_FRAMES) {
        return refuse(CookedAudioStatus::CapExceeded, std::format("the header declares {} frames, over the cap of {}",
                                                                  frameCount, MAX_COOKED_AUDIO_FRAMES));
    }

    // 8. the sample count, COMPUTED IN u64. At the caps (8 x 28 800 000 = 230 400 000) the product
    //    fits a u32, but a HOSTILE header can claim 0xFFFFFFFF for both, and that product overflows a
    //    u32 to 1 -- a file that would then pass every size check with a 66-byte buffer. The two
    //    factors are already inside their own caps here, so this cap is the multi-channel one biting
    //    (docs/09 section 14.1): 8 channels caps at 7 200 000 frames, not at 28 800 000.
    const std::uint64_t sampleCount = static_cast<std::uint64_t>(channels) * frameCount;
    if (sampleCount > MAX_COOKED_AUDIO_SAMPLES) {
        return refuse(CookedAudioStatus::CapExceeded,
                      std::format("{} channels x {} frames is {} samples, over the cap of {}", channels, frameCount,
                                  sampleCount, MAX_COOKED_AUDIO_SAMPLES));
    }

    // 9. the region offset, EXACTLY. Unlike the parser's permissiveness elsewhere, and for docs/09
    //    section 13.2's stated reason one format over: this format has exactly ONE legal layout, and
    //    equality is the only check that can see a MISPOSITIONED region at all. v1 carries the field
    //    rather than assuming it so a v2 can move the region without a new header shape.
    const std::uint64_t sampleDataOffset = getU64(bytes, H_SAMPLE_DATA_OFFSET);
    if (sampleDataOffset != COOKED_AUDIO_HEADER_BYTES) {
        return refuse(CookedAudioStatus::BadRange,
                      std::format("the sample region begins at offset {}, and v1 puts it at exactly {}",
                                  sampleDataOffset, COOKED_AUDIO_HEADER_BYTES));
    }

    // 10/11. BOTH totalBytes identities, as TWO comparisons with TWO distinguishable messages --
    //        SizeMismatch cannot tell them apart by status, so the wording is what separates them.
    //        They are genuinely independent: a buffer resized to match a wrong totalBytes satisfies
    //        11 and fails 10, and a truncated buffer with a right totalBytes fails 11 and passes 10.
    const std::uint64_t totalBytes = getU64(bytes, H_TOTAL_BYTES);
    const std::uint64_t expected = COOKED_AUDIO_HEADER_BYTES + (COOKED_AUDIO_SAMPLE_BYTES * sampleCount);
    if (totalBytes != expected) {
        return refuse(CookedAudioStatus::SizeMismatch,
                      std::format("the header declares {} total bytes but {} channels x {} frames need {}", totalBytes,
                                  channels, frameCount, expected));
    }
    if (totalBytes != bytes.size()) {
        return refuse(
            CookedAudioStatus::SizeMismatch,
            std::format("the header declares {} total bytes but the buffer holds {}", totalBytes, bytes.size()));
    }

    // 12. defence in depth. Unreachable while the three caps above hold -- the sample cap already
    //     bounds `expected` -- and it stays because MAX_COOKED_AUDIO_BYTES is what every consumer
    //     outside this file sizes its refusals against (audio::loadAudioClip's TooLarge, in
    //     particular), so the parser must never be the one place that can return an Ok above it.
    if (expected > MAX_COOKED_AUDIO_BYTES) {
        return refuse(CookedAudioStatus::CapExceeded,
                      std::format("the file needs {} bytes, over the cap of {}", expected, MAX_COOKED_AUDIO_BYTES));
    }

    CookedAudioParseResult out;
    out.audio.formatVersion = formatVersion;
    out.audio.cookerVersion = getU32(bytes, H_COOKER_VERSION);
    out.audio.sourceGuid.hi = getU64(bytes, H_GUID_HI);
    out.audio.sourceGuid.lo = getU64(bytes, H_GUID_LO);
    out.audio.sampleRate = sampleRate;
    out.audio.channels = channels;
    out.audio.frameCount = frameCount;
    out.audio.sampleDataOffset = sampleDataOffset;
    out.audio.totalBytes = totalBytes;
    out.audio.bytes = bytes;  // THE SPAN IS THE BUFFER. Never a copy -- see the header's lifetime note.
    return out;
}

}  // namespace engine::assets
