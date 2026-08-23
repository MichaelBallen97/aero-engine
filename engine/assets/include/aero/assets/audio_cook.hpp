#pragma once
// Aero Engine — the audio cook (task 3.7.1): interleaved s16 PCM in, .aerowave v1 bytes out. The
// PRODUCER half of the cooked audio container; cooked_audio.hpp is the FORMAT half, and a runtime
// that never cooks anything needs only that one.
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro,
// AND NO FLOATING POINT AT ALL — not an arithmetic operation, not a comparison, not a local. The
// decoders emit s16 and this file validates integers and calls putU16; the ONE float that appears
// anywhere is AudioCookStats::durationSeconds, a REPORTED statistic computed by
// cookedAudioDurationSeconds and never written to a byte. That is a stronger property than .aeromesh
// or .aeroanim have (both move float data bit for bit) and it is closer to INV-T4's texture rule.
// INV-A1 is a claim about cooked_audio.{hpp,cpp} and audio_cook.{hpp,cpp} — exactly the scope that is
// true.
//
// THE COOK CONVERTS NOTHING. No resampling, no downmix, no upmix, no channel reorder, no
// normalization, no trimming, no silence stripping, no fade, no loop points, no dithering — and NO
// SETTING FOR ANY OF THEM. There is no AudioCookSettings type, for the same reason there is no
// MeshCookSettings, no TextureCookSettings and no AnimationCookSettings: an empty settings struct is
// a shape that invites a field. Whatever the decoder produced is what lands in the file, sample for
// sample.
#include <aero/assets/cooked_audio.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::assets {

struct AudioCookInput {
    Guid sourceGuid;  // may be nil; nil is legal and deterministic
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    // Interleaved FRAME-MAJOR (docs/09 section 14.2), EXACTLY channels * frameCount entries. There is
    // deliberately no frameCount here: see the refusal list in the .cpp — two numbers that must agree
    // are one number, and this one is DERIVED.
    std::span<const std::int16_t> samples;
};

struct AudioCookStats {
    std::uint32_t frameCount = 0;
    std::uint64_t sampleCount = 0;
    std::uint64_t byteSize = 0;
    // Via cookedAudioDurationSeconds — THE one spelling, used rather than restated. The only float in
    // this whole producer, and it is a statistic a caller may want to print.
    float durationSeconds = 0.0F;
};

// BINARY, the TextureCookStatus shape rather than the mesh cook's or the animation cook's
// three-valued one, for the reason texture_cook.hpp:47-52 records one format over: a sound cannot be
// partially cooked — there is no "some of the waveform" — so `bytes` is empty IFF Refused, which is a
// stronger and simpler invariant, and the CLI's error path never has to distinguish "coherent but
// smaller" from "nothing".
enum class AudioCookStatus : std::uint8_t { Ok = 0, Refused };

struct AudioCookResult {
    AudioCookStatus status = AudioCookStatus::Ok;
    std::string message;  // "" IFF Ok; every refusal names the offending value AND its bound
    // v1's cook emits NO warning at all. The vector and its uncapped total exist because the shape is
    // the house's and because the first thing that wants one should not have to change the result
    // type. Do NOT write a synthetic warning to make the field look used. In particular, a 24-bit or
    // 32-bit-float source being quantized to s16 emits NO WARNING: s16 is the format's declared
    // precision (docs/09 section 14.0), not a defect, and a warning that fires on the correct,
    // intended behaviour of the only format v1 has is noise — the same rule 3.6.1's RenderView
    // counters recorded ("unresolved is transient by design, so a WARN would fire once per session on
    // correct behaviour").
    std::vector<std::string> warnings;  // capped at MAX_COOK_WARNINGS, reused from cooked_mesh.hpp
    std::size_t warningTotal = 0;       // UNCAPPED
    std::vector<std::byte> bytes;       // EMPTY IFF status == Refused
    AudioCookStats stats;
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. NO FLOATING POINT.
//
// validate rate -> validate channels -> validate divisibility -> DERIVE frameCount -> validate
// frameCount and the sample count against their caps -> size a zero-initialized buffer -> write.
//
// THE CAPS ARE CHECKED BEFORE THE BUFFER IS SIZED, so an over-long input costs NO allocation. That is
// the "sort before cap" rule's sibling: VALIDATE BEFORE RESERVE.
[[nodiscard]] AudioCookResult cookAudio(const AudioCookInput& input);

}  // namespace engine::assets
