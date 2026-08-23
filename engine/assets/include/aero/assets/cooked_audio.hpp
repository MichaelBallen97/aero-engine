#pragma once
// Aero Engine — the cooked audio clip container v1 (.aerowave, task 3.7.1). The tree's FOURTH
// first-party binary format, and the simplest of the four by a wide margin: one header and one bulk
// region, no record table, no code tables and NO PADDING SITE ANYWHERE.
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro,
// and — uniquely in this subsystem — NO FLOATING POINT IN THE WRITER PATH AT ALL. The decoders emit
// s16; the cook validates integers and calls putU16. That is a STRONGER property than .aeromesh or
// .aeroanim have (both are float data moved bit for bit) and it is closer to INV-T4's texture rule.
// It is a claim about cooked_audio.{hpp,cpp} and audio_cook.{hpp,cpp}, which is exactly the scope
// that is true — INV-T4 was mis-stated once already as a claim about engine/assets as a whole, which
// is and always was false (mesh_cook.cpp includes <cmath>). The ONE float-returning function here is
// cookedAudioDurationSeconds, a READER-side convenience that never touches a byte.
//
// The normative specification of this format is docs/09-file-formats.md section 14. THIS HEADER IS
// NOT THE SPEC — if the two ever disagree, docs/09 wins and one of them is a bug. The
// COOKED_AUDIO_*_BYTES constants below are the ONLY sizes: `sizeof` is never taken of an on-disk
// record anywhere in this subsystem, because a struct's size is a compiler's opinion and a format's
// is not.
//
// INCLUDES, and the recorded reconciliation behind them: the eight put*/get* primitives live in
// cooked_mesh.hpp, so this header includes it with the identical one-line comment
// cooked_texture.hpp:24 has carried since 3.3.2, cooked_skeleton.hpp:20 since 3.5.1 and
// cooked_animation.hpp:21 since 3.5.2. A core-and-standard-library-only include list and "bytes are
// formed exclusively through the eight primitives" cannot both hold, and the eight-places rule wins,
// for the fourth format running. Note that unlike the other three this file does NOT include
// <aero/core/math.hpp>: there is no Vec3, no Quat and no Mat4 anywhere in this format.
#include <aero/assets/cooked_mesh.hpp>  // the eight byte primitives + their endianness static_assert
#include <aero/core/guid.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// ---- identity ---------------------------------------------------------------------------------
inline constexpr std::string_view COOKED_AUDIO_MAGIC = "AEROWAVE";  // 8 ASCII bytes, no NUL
inline constexpr std::uint32_t COOKED_AUDIO_FORMAT_VERSION = 1;
// "The same input now cooks to different bytes", and nothing else. It NEVER gates a parse — see
// cooked_mesh.hpp's note on the distinction, which is the same one docs/09 section 9.11 states.
inline constexpr std::uint32_t COOKED_AUDIO_COOKER_VERSION = 1;

// ---- sizes. The ONLY sizes. ---------------------------------------------------------------------
// 64 is a multiple of 16, so the sample region starts 16-aligned and is therefore trivially 2-aligned
// for an s16. THAT DOES NOT LICENSE A TYPED SPAN OVER IT — every read goes through getU16, for the
// reason cooked_animation.hpp:154-161 states verbatim: a file's region carries no alignment guarantee
// this program's types satisfy, and a typed span would need a reinterpret_cast, which is one of the
// greps this subsystem must keep returning prose only.
//
// ZERO PADDING SITES, and that is a contract rather than an accident. .aeroanim has exactly one and
// docs/09 section 13.1 spends a paragraph on it because it has two bulk regions. This format has ONE
// bulk region and it is LAST, so totalBytes == 64 + 2 * channels * frameCount EXACTLY, with no
// rounding term anywhere. There is no padding function to write, no padding byte to zero and no
// padding site for a parser to check.
inline constexpr std::size_t COOKED_AUDIO_HEADER_BYTES = 64;
inline constexpr std::size_t COOKED_AUDIO_ALIGNMENT = 16;
inline constexpr std::size_t COOKED_AUDIO_SAMPLE_BYTES = 2;  // s16 IS formatVersion 1 (docs/09 14.0)
static_assert(COOKED_AUDIO_HEADER_BYTES % COOKED_AUDIO_ALIGNMENT == 0);

// ---- caps. Enforced by BOTH the writer and the parser. -----------------------------------------
// The two rate bounds are miniaudio's own standard-rate bounds, taken rather than invented:
// ma_standard_sample_rate_min = ma_standard_sample_rate_8000 and ma_standard_sample_rate_max =
// ma_standard_sample_rate_384000 (miniaudio.h:4342-4343 in the pinned 0.11.25 tree), which its device
// layer clamps against at :25549-25553. A file this engine refuses is then a file no device this
// engine can open would have accepted anyway.
inline constexpr std::uint32_t MIN_COOKED_AUDIO_SAMPLE_RATE = 8000;    // ma_standard_sample_rate_min
inline constexpr std::uint32_t MAX_COOKED_AUDIO_SAMPLE_RATE = 384000;  // ma_standard_sample_rate_max
inline constexpr std::uint32_t MAX_COOKED_AUDIO_CHANNELS = 8;
inline constexpr std::uint32_t MAX_COOKED_AUDIO_FRAMES = 28800000;  // 10 minutes at 48 kHz
// THE SAMPLE CAP IS THE BINDING ONE FOR MULTI-CHANNEL, BY CONSTRUCTION AND ON PURPOSE. 8-channel
// audio caps at 7 200 000 frames (2 min 30 s at 48 kHz) rather than at 28 800 000, because the thing
// worth bounding is RESIDENT BYTES, not seconds.
//
// The static_assert below is an expression against the same expression DELIBERATELY -- and MEASURED,
// by task 3.7.1's sabotage matrix (seed A14), it is a DRIFT tripwire rather than a
// literal-substitution one. Replacing the definition with the CORRECT literal 57600000 compiles
// clean, because 57600000 == 2 * 28800000. What fires the assertion is that literal going STALE the
// day MAX_COOKED_AUDIO_FRAMES moves and the second number does not -- which is the failure mode worth
// catching, and the one this expression exists to prevent. A static_assert failure is a build
// failure, which is a stronger witness than any test; with the assertion deleted as well, a wrong
// literal is caught at run time by CA3, CA4 and CA24.
inline constexpr std::uint64_t MAX_COOKED_AUDIO_SAMPLES = 2ULL * MAX_COOKED_AUDIO_FRAMES;
static_assert(MAX_COOKED_AUDIO_SAMPLES == 2ULL * MAX_COOKED_AUDIO_FRAMES,
              "the sample cap is STEREO AT THE FULL LENGTH, never a second literal");
// DERIVED from the header size and the sample cap, so it cannot drift: 115 200 064 B, ~109.9 MiB.
inline constexpr std::uint64_t MAX_COOKED_AUDIO_BYTES =
    COOKED_AUDIO_HEADER_BYTES + (COOKED_AUDIO_SAMPLE_BYTES * MAX_COOKED_AUDIO_SAMPLES);

// ---- the header's field offsets, named ONCE for the whole format --------------------------------
// docs/09 section 14.1 is the normative table; these mirror it, and NOTHING in this subsystem spells
// an .aerowave header offset as a literal.
//
// THEY LIVE IN A PUBLIC `detail` NAMESPACE RATHER THAN IN A .cpp's ANONYMOUS ONE, unlike the other
// three containers, for one reason: this format's WRITER (audio_cook.cpp) and its PARSER
// (cooked_audio.cpp) are two translation units, and an anonymous namespace cannot be shared across
// them. A second copy of these ten numbers is exactly the disguise a swapped-offset defect wears, so
// there is one copy and both TUs reach it. The texture_cook.hpp `detail` seam is the precedent for a
// public detail namespace in this subsystem; a consumer outside engine/assets and aero_tests reaching
// into it is a review finding, not a supported use.
namespace detail {
inline constexpr std::size_t H_MAGIC = 0;
inline constexpr std::size_t H_FORMAT_VERSION = 8;
inline constexpr std::size_t H_COOKER_VERSION = 12;
inline constexpr std::size_t H_GUID_HI = 16;
inline constexpr std::size_t H_GUID_LO = 24;
inline constexpr std::size_t H_SAMPLE_RATE = 32;
inline constexpr std::size_t H_CHANNELS = 36;
inline constexpr std::size_t H_FRAME_COUNT = 40;
inline constexpr std::size_t H_RESERVED_FLAGS = 44;
inline constexpr std::size_t H_SAMPLE_DATA_OFFSET = 48;
inline constexpr std::size_t H_TOTAL_BYTES = 56;
static_assert(H_TOTAL_BYTES + 8 == COOKED_AUDIO_HEADER_BYTES);
}  // namespace detail

// THE duration, spelled ONCE for the container, the cook, the loader and every future consumer, so no
// two callers can disagree and no field on disk can contradict its own inputs (docs/09 section 14.0).
// The header stores NO durationSeconds, unlike .aeroanim, because an audio clip's duration is EXACTLY
// frameCount / sampleRate — storing it would create a field that can disagree with its own inputs in
// a file a hostile or hand-edited producer controls, and the parser would then have to decide which
// one wins.
//
// TOTAL: a zero sampleRate — which parse refuses and this function does not — answers 0.0F rather
// than dividing. This is the ONE float-returning function in this header and it never touches a byte.
[[nodiscard]] constexpr float cookedAudioDurationSeconds(std::uint32_t sampleRate, std::uint32_t frameCount) noexcept {
    return sampleRate == 0 ? 0.0F : static_cast<float>(frameCount) / static_cast<float>(sampleRate);
}

// LIFETIME, verbatim from CookedAnimation because it is verbatim the same contract: `bytes` IS the
// buffer handed to parseCookedAudio, retained as a span. The sample region is NEVER COPIED — that is
// the whole promise of this format, and at up to 115 MB it is the promise that matters most in this
// subsystem. A CookedAudio outliving its buffer is a dangling read; audio::AudioClip is the type that
// exists so no consumer has to manage that by hand. Nothing else should index `bytes` by hand — the
// two accessors below are the sanctioned way to reach the samples.
struct CookedAudio {
    std::uint32_t formatVersion = 0;
    std::uint32_t cookerVersion = 0;
    Guid sourceGuid;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t frameCount = 0;
    std::uint64_t sampleDataOffset = 0;  // exactly COOKED_AUDIO_HEADER_BYTES in v1
    std::uint64_t totalBytes = 0;
    std::span<const std::byte> bytes;
};

// TOTAL on a CookedAudio parseCookedAudio returned Ok for: exactly
// COOKED_AUDIO_SAMPLE_BYTES * channels * frameCount bytes, starting at sampleDataOffset. An
// inconsistent CookedAudio yields an EMPTY span rather than a read — a caller bug must not become one.
[[nodiscard]] std::span<const std::byte> audioSampleBytes(const CookedAudio& audio) noexcept;

// The ONE typed reader. Takes a BYTE span for cooked_animation.hpp's stated reason. The index is
// SAMPLE-relative (frame f, channel c is at f * channels + c — see the frame-major note below); an
// out-of-range index answers 0 rather than reading, because silence is the right answer to a caller
// bug in an audio path and it must never become a read. getU16 is already total
// (cooked_mesh.hpp:201-207 returns 0 for an out-of-range offset), so that totality is INHERITED here
// rather than re-implemented.
//
// All three target hosts are 64-bit, so `index * 2` cannot overflow a std::size_t at any index a
// std::span could describe; there is deliberately no guard for a case nothing can reach.
[[nodiscard]] constexpr std::int16_t audioSample(std::span<const std::byte> samples, std::uint64_t index) noexcept {
    return static_cast<std::int16_t>(getU16(samples, static_cast<std::size_t>(index) * COOKED_AUDIO_SAMPLE_BYTES));
}

// FRAME-MAJOR INTERLEAVING IS NORMATIVE (docs/09 section 14.2): frame f, channel c is at sample index
// f * channels + c. There is NO planar layout and no flag that could select one.

enum class CookedAudioStatus : std::uint8_t {
    Ok = 0,
    TooSmall,            // shorter than the header
    BadMagic,            //
    UnsupportedVersion,  //
    ReservedNotZero,     // reservedFlags — a REFUSAL, never an ignore
    SizeMismatch,        // totalBytes != bytes.size(), or != 64 + 2 * channels * frameCount
    CapExceeded,         // any of the five caps
    BadTable,            // a zero count, or a sample rate outside [8000, 384000]
    BadRange,            // sampleDataOffset is not exactly COOKED_AUDIO_HEADER_BYTES
};
// A switch with NO `default:` (the cookedMeshStatusLabel precedent) — a future enumerator is a
// -Wswitch failure on the Linux lane rather than a silent fallthrough. NOT named toString: an engine
// toString(SomeEnum) is found by ADL inside doctest's stringifier, beats doctest's own template, and
// makes the decomposer try std::string_view + const char* — a hard compile error on EVERY lane,
// reported inside doctest.h rather than at the CHECK. 3.6.3 hit it for real on rhi::TextureFormat.
[[nodiscard]] std::string_view cookedAudioStatusLabel(CookedAudioStatus status) noexcept;

struct CookedAudioParseResult {
    CookedAudioStatus status = CookedAudioStatus::Ok;
    std::string message;  // "" IFF Ok; names the offending value AND its bound wherever there is one
    CookedAudio audio;    // meaningful only when Ok
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. Written to the importer's hostile-input standard,
// because at Phase 5 this reads bytes out of a .pak that may have been shipped, patched, truncated by
// a failed download, or crafted: every range check is a SUBTRACTION against the known-good size,
// never an addition that can wrap, and nothing is reserved before the header's counts have passed
// their caps. (Nothing is reserved here AT ALL — the region is a span, never a copy — which makes
// that trivially true and is worth saying so nobody adds a copy later.)
//
// A .aerowave is NEVER EMPTY — section 12.0's asymmetry inherited a third time, AT PARSE: sampleRate
// >= 8000, channels >= 1 and frameCount >= 1. The cook is per-FILE, so a source that decodes to zero
// frames produces NO ARTIFACT AT ALL and a CLI error. A clip with no samples is not a degenerate
// sound; it is the absence of one. Do not "relax" this to match the mesh container.
[[nodiscard]] CookedAudioParseResult parseCookedAudio(std::span<const std::byte> bytes);

}  // namespace engine::assets
