#pragma once
// Aero Engine — the cooked animation clip container v1 (.aeroanim, task 3.5.2). The tree's THIRD
// first-party binary format, and a SIBLING of both .aeromesh and .aeroskel rather than a region
// inside either: a clip is a property of a MOTION, a rig is a property of a SKIN, and one rig has
// many clips. Neither file names the other -- the binding is the consumer's, resolved through
// docs/09 section 12.3's sourceNodeLocalId (section 13.0).
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro.
//
// The normative specification of this format is docs/09-file-formats.md section 13. THIS HEADER IS
// NOT THE SPEC -- if the two ever disagree, docs/09 wins and one of them is a bug. The two
// COOKED_ANIMATION_*_BYTES constants below are the ONLY sizes: `sizeof` is never taken of an on-disk
// record anywhere in this subsystem, because a struct's size is a compiler's opinion and a format's
// is not.
//
// INCLUDES, and the recorded reconciliation behind them: the eight put*/get* primitives live in
// cooked_mesh.hpp, so this header includes it with the identical one-line comment
// cooked_texture.hpp:24 has carried since 3.3.2 and cooked_skeleton.hpp:20 since 3.5.1. The
// eight-places rule wins over any core-and-standard-library-only reading, exactly as it did for both
// previous formats. Nothing else is included, ever.
#include <aero/assets/cooked_mesh.hpp>  // the eight byte primitives + their endianness static_assert
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// ---- identity ---------------------------------------------------------------------------------
inline constexpr std::string_view COOKED_ANIMATION_MAGIC = "AEROANIM";  // 8 ASCII bytes, no NUL
inline constexpr std::uint32_t COOKED_ANIMATION_FORMAT_VERSION = 1;
// "The same input now cooks to different bytes", and nothing else. It NEVER gates a parse -- see
// cooked_mesh.hpp's note on the distinction, which is the same one docs/09 section 9.11 states.
inline constexpr std::uint32_t COOKED_ANIMATION_COOKER_VERSION = 1;

// ---- record sizes. The ONLY sizes. --------------------------------------------------------------
// 80 and 32 are both multiples of 16, so the channel table both STARTS and ENDS 16-aligned and this
// format needs no padding after it, ever. Its ONE padding site is between the times and values
// regions (docs/09 section 13.1).
inline constexpr std::size_t COOKED_ANIMATION_HEADER_BYTES = 80;
inline constexpr std::size_t COOKED_ANIMATION_CHANNEL_BYTES = 32;
inline constexpr std::size_t COOKED_ANIMATION_ALIGNMENT = 16;
static_assert(COOKED_ANIMATION_HEADER_BYTES % COOKED_ANIMATION_ALIGNMENT == 0);
static_assert(COOKED_ANIMATION_CHANNEL_BYTES % COOKED_ANIMATION_ALIGNMENT == 0);

// ---- the frozen code tables (docs/09 section 13.6) ----------------------------------------------
// Deliberately independent of editor::AnimationPath and editor::AnimationInterpolation, which live
// in a layer engine/assets may never include; the correspondence is asserted in the EDITOR test
// tier, the one place both are visible -- the CookedVertexSemantic precedent. An unknown code is a
// PARSE REFUSAL, never a reinterpretation. The width is the FORMAT's, not a size choice, which is
// why the NOLINT is here and says so.
// NOLINTNEXTLINE(performance-enum-size)
enum class CookedAnimationPath : std::uint16_t { Translation = 0, Rotation = 1, Scale = 2 };
// NOLINTNEXTLINE(performance-enum-size)
enum class CookedAnimationInterpolation : std::uint16_t { Linear = 0, Step = 1, CubicSpline = 2 };

// Every u16 is a value of a fixed-underlying-type enum, so static_cast<CookedAnimationPath>(7) is
// well-formed -- the CookedTextureFormat lesson. BOTH halves validate; the cook's half is not
// optional.
[[nodiscard]] constexpr bool isCookedAnimationPath(CookedAnimationPath path) noexcept {
    return path == CookedAnimationPath::Translation || path == CookedAnimationPath::Rotation ||
           path == CookedAnimationPath::Scale;
}
[[nodiscard]] constexpr bool isCookedAnimationInterpolation(CookedAnimationInterpolation interpolation) noexcept {
    return interpolation == CookedAnimationInterpolation::Linear ||
           interpolation == CookedAnimationInterpolation::Step ||
           interpolation == CookedAnimationInterpolation::CubicSpline;
}

// THE cubic multiplier, spelled ONCE for the writer, the parser, the sampler and the caps below.
// Total by construction: an out-of-range code answers 1, and every caller checks the code first.
[[nodiscard]] constexpr std::uint32_t cookedAnimationValuesPerKey(CookedAnimationInterpolation interpolation) noexcept {
    return interpolation == CookedAnimationInterpolation::CubicSpline ? 3U : 1U;
}

// THE format's one padding site, spelled once so the writer and the parser cannot disagree about it.
// 0, 12, 8 or 4 bytes for keyCount % 4 == 0, 1, 2, 3.
[[nodiscard]] constexpr std::uint32_t cookedAnimationTimesPadding(std::uint32_t keyCount) noexcept {
    const std::uint64_t over = (4ULL * keyCount) % 16ULL;
    return over == 0ULL ? 0U : static_cast<std::uint32_t>(16ULL - over);
}

// Switches with NO `default:` (the cookedMeshStatusLabel precedent). NOT named toString: an engine
// toString(SomeEnum) is found by ADL inside doctest's stringifier and breaks every lane.
[[nodiscard]] std::string_view cookedAnimationPathLabel(CookedAnimationPath path) noexcept;
[[nodiscard]] std::string_view cookedAnimationInterpolationLabel(CookedAnimationInterpolation interpolation) noexcept;

// ---- caps. Enforced by BOTH the writer and the parser. -----------------------------------------
// 4096 channels: one channel per path per joint at section 12.5's 1024-record cap is 3072, and the
// headroom is for channels targeting non-joint nodes (glTF clips routinely animate cameras and mesh
// nodes alongside joints). 2 000 000 keys MIRRORS the importer's MAX_ANIMATION_KEYS_PER_MODEL, so
// this cook is never narrower than the importer that feeds it.
inline constexpr std::uint32_t MAX_COOKED_ANIMATION_CHANNELS = 4096;
inline constexpr std::uint32_t MAX_COOKED_ANIMATION_KEYS = 2000000;
inline constexpr std::uint32_t MAX_COOKED_ANIMATION_VALUES = 3 * MAX_COOKED_ANIMATION_KEYS;
static_assert(MAX_COOKED_ANIMATION_VALUES ==
                  MAX_COOKED_ANIMATION_KEYS * cookedAnimationValuesPerKey(CookedAnimationInterpolation::CubicSpline),
              "the value cap is the key cap times THE cubic multiplier, never a second literal");
// DERIVED from the other three plus the record sizes, so it cannot drift: 104 131 164 B, ~99.3 MiB.
inline constexpr std::uint64_t MAX_COOKED_ANIMATION_BYTES =
    COOKED_ANIMATION_HEADER_BYTES +
    (static_cast<std::uint64_t>(COOKED_ANIMATION_CHANNEL_BYTES) * MAX_COOKED_ANIMATION_CHANNELS) +
    (4ULL * MAX_COOKED_ANIMATION_KEYS) + (COOKED_ANIMATION_ALIGNMENT - 4ULL) + (16ULL * MAX_COOKED_ANIMATION_VALUES);

// ---- the parsed records -------------------------------------------------------------------------
struct CookedAnimationChannel {
    // The source node's localId -- matched against section 12.3's sourceNodeLocalId by the consumer.
    // NEVER a joint index and never a position: a clip must survive re-cooking its rig.
    std::uint32_t targetNodeLocalId = 0;
    CookedAnimationPath path = CookedAnimationPath::Translation;
    CookedAnimationInterpolation interpolation = CookedAnimationInterpolation::Linear;
    std::uint32_t keyCount = 0;    // >= 1
    std::uint32_t firstKey = 0;    // index into the times region, IN KEYS
    std::uint32_t firstValue = 0;  // index into the values region, IN VALUES
    std::uint32_t valueCount = 0;  // == keyCount * cookedAnimationValuesPerKey(interpolation)
};

// LIFETIME, verbatim from CookedMesh because it is verbatim the same contract: `bytes` IS the buffer
// handed to parseCookedAnimation, retained as a span. The channel table is an OWNED copy (bounded by
// the cap, so always small); the two bulk regions are NEVER copied -- that is the whole promise of
// this format. A CookedAnimation outliving its buffer is a dangling read, and the only defence is
// this comment plus the two accessors below, which are the sanctioned way to reach bulk data.
// Nothing else should index `bytes` by hand.
//
// Unlike CookedSkeleton, which is fully owned: that format has no bulk region and a maximal file is
// 131 KB, so ownership was cheaper than a lifetime rule. This one has two bulk regions and no small
// bound, so the same rule decides it the other way.
struct CookedAnimation {
    std::uint32_t formatVersion = 0;
    std::uint32_t cookerVersion = 0;
    Guid sourceGuid;
    std::uint32_t sourceAnimationIndex = 0;  // the POSITION in ImportedModel::animations
    float durationSeconds = 0.0F;
    std::uint32_t keyCount = 0;  // the TIMES REGION's size in keys -- NOT necessarily the channel sum
    std::uint32_t valueCount = 0;
    std::uint64_t timesDataOffset = 0;
    std::uint64_t valuesDataOffset = 0;
    std::vector<CookedAnimationChannel> channels;
    std::span<const std::byte> bytes;
};

// Both TOTAL on an animation parseCookedAnimation returned Ok for. An out-of-range channel index
// yields an EMPTY span rather than a read -- a caller bug must not become one.
[[nodiscard]] std::span<const std::byte> channelTimeBytes(const CookedAnimation& animation,
                                                          std::uint32_t channelIndex) noexcept;
[[nodiscard]] std::span<const std::byte> channelValueBytes(const CookedAnimation& animation,
                                                           std::uint32_t channelIndex) noexcept;

// The two typed readers over those spans. Bytes -> float / Vec4 through getF32 ONLY.
// THEY TAKE BYTE SPANS RATHER THAN std::span<const float> / std::span<const Vec4> ON PURPOSE, and the
// reason is not style: a file's regions carry no alignment guarantee this program's types satisfy, so
// a typed span over them would need a reinterpret_cast -- a strict-aliasing violation, and on the
// values region an UNDER-ALIGNED Vec4. .aeromesh's two accessors return byte spans for exactly this
// reason, and reinterpret_cast is one of the greps this subsystem must keep returning prose only.
// Indices are CHANNEL-LOCAL: the spans above are already offset by firstKey/firstValue, which is why
// those two fields appear exactly once each in this whole subsystem.
[[nodiscard]] constexpr float animationKeyTime(std::span<const std::byte> times, std::uint32_t k) noexcept {
    return getF32(times, static_cast<std::size_t>(k) * 4U);
}
[[nodiscard]] constexpr Vec4 animationKeyValue(std::span<const std::byte> values, std::uint32_t v) noexcept {
    const std::size_t at = static_cast<std::size_t>(v) * 16U;
    return Vec4{getF32(values, at), getF32(values, at + 4), getF32(values, at + 8), getF32(values, at + 12)};
}

enum class CookedAnimationStatus : std::uint8_t {
    Ok = 0,
    TooSmall,            // shorter than the header
    BadMagic,            //
    UnsupportedVersion,  //
    ReservedNotZero,     // reservedFlags, a channel's reserved0, or a padding byte -- a REFUSAL
    SizeMismatch,        // totalBytes != the buffer's size, or != valuesDataOffset + 16 * valueCount
    CapExceeded,         //
    BadTable,            // a zero count, an unknown code, or valueCount != keyCount * the multiplier
    BadRange,            // a region offset, the padding site, or a channel slice outside its region
};
[[nodiscard]] std::string_view cookedAnimationStatusLabel(CookedAnimationStatus status) noexcept;

struct CookedAnimationParseResult {
    CookedAnimationStatus status = CookedAnimationStatus::Ok;
    std::string message;        // "" IFF Ok; names the offending channel index wherever there is one
    CookedAnimation animation;  // meaningful only when Ok
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. Written to the importer's hostile-input standard,
// because at Phase 5 this reads bytes out of a .pak that may have been shipped, patched, truncated
// by a failed download, or crafted: every range check is a SUBTRACTION against the known-good size,
// never an addition that can wrap, and nothing is reserved before the header's counts have passed
// their caps.
//
// A .aeroanim is NEVER EMPTY -- section 12.0's asymmetry inherited one format over. channelCount,
// keyCount, valueCount and every channel's keyCount are all >= 1 at parse, because the cook is
// per-CLIP and a model with no usable channel produces no artifact at all and a CLI error. A clip
// with nothing left is not a degenerate animation; it is the absence of one.
[[nodiscard]] CookedAnimationParseResult parseCookedAnimation(std::span<const std::byte> bytes);

}  // namespace engine::assets
