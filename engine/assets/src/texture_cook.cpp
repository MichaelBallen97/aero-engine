// Aero Engine — the texture cook (task 3.3.2): the committed gamma tables, the integer polyphase mip
// filter and (from step 5) cookTexture itself. See texture_cook.hpp for the contract and
// docs/09-file-formats.md section 10.8 for the normative filter and table definitions.
//
// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. NO FLOATING POINT.
#include <aero/assets/bc_block.hpp>
#include <aero/assets/texture_cook.hpp>
#include <aero/core/guid.hpp>
#include <aero/core/profiler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::assets::detail {
namespace {

// ---- the two committed gamma tables --------------------------------------------------------------
//
// SRGB_TO_LINEAR[v] = round(65535 * eotf(v / 255)) for v in 0..255, where eotf is the sRGB
// electro-optical transfer function
//     c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
// Generated ONCE by a ~15-line Python 3 script whose source is recorded verbatim in
// docs/10-engineering-log.md's entry for this task, so anyone can re-derive rather than trust. It is
// deliberately NOT a build target, a CMake step or a tool: a generator that lives in the build is a
// generator somebody eventually runs at configure time, and the table's values then depend on the
// build machine's libm -- which is the exact hazard the committed table exists to avoid.
//
// No entry lands on an exact half, so round-half-up and round-half-to-even agree everywhere here and
// the tie rule is moot. Verified while generating, not assumed.
constexpr std::array<std::uint16_t, 256> SRGB_TO_LINEAR = {
    0,     20,    40,    60,    80,    99,    119,   139,   159,   179,   199,   219,   241,   264,   288,   313,
    340,   367,   396,   427,   458,   491,   526,   562,   599,   637,   677,   718,   761,   805,   851,   898,
    947,   997,   1048,  1101,  1156,  1212,  1270,  1330,  1391,  1453,  1517,  1583,  1651,  1720,  1790,  1863,
    1937,  2013,  2090,  2170,  2250,  2333,  2418,  2504,  2592,  2681,  2773,  2866,  2961,  3058,  3157,  3258,
    3360,  3464,  3570,  3678,  3788,  3900,  4014,  4129,  4247,  4366,  4488,  4611,  4736,  4864,  4993,  5124,
    5257,  5392,  5530,  5669,  5810,  5953,  6099,  6246,  6395,  6547,  6700,  6856,  7014,  7174,  7335,  7500,
    7666,  7834,  8004,  8177,  8352,  8528,  8708,  8889,  9072,  9258,  9445,  9635,  9828,  10022, 10219, 10417,
    10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909,
    14146, 14387, 14629, 14874, 15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
    23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325, 25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094,
    28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
    41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359,
    48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
};

// LINEAR_TO_SRGB_THRESHOLDS[i] = the midpoint between SRGB_TO_LINEAR[i] and SRGB_TO_LINEAR[i + 1],
// rounded HALF UP: (SRGB_TO_LINEAR[i] + SRGB_TO_LINEAR[i + 1] + 1) / 2, for i in 0..254.
//
// This table is NOT independently trusted: the static_assert below CHECKS it against the forward
// table at compile time, so only ONE of the two arrays can be wrong -- and that one is then covered
// by five hand-recomputed anchors and by the exhaustive round trip.
constexpr std::array<std::uint16_t, 255> LINEAR_TO_SRGB_THRESHOLDS = {
    10,    30,    50,    70,    90,    109,   129,   149,   169,   189,   209,   230,   253,   276,   301,   327,
    354,   382,   412,   443,   475,   509,   544,   581,   618,   657,   698,   740,   783,   828,   875,   923,
    972,   1023,  1075,  1129,  1184,  1241,  1300,  1361,  1422,  1485,  1550,  1617,  1686,  1755,  1827,  1900,
    1975,  2052,  2130,  2210,  2292,  2376,  2461,  2548,  2637,  2727,  2820,  2914,  3010,  3108,  3208,  3309,
    3412,  3517,  3624,  3733,  3844,  3957,  4072,  4188,  4307,  4427,  4550,  4674,  4800,  4929,  5059,  5191,
    5325,  5461,  5600,  5740,  5882,  6026,  6173,  6321,  6471,  6624,  6778,  6935,  7094,  7255,  7418,  7583,
    7750,  7919,  8091,  8265,  8440,  8618,  8799,  8981,  9165,  9352,  9540,  9732,  9925,  10121, 10318, 10518,
    10721, 10925, 11132, 11341, 11552, 11766, 11982, 12200, 12420, 12642, 12867, 13095, 13325, 13557, 13791, 14028,
    14267, 14508, 14752, 14998, 15247, 15497, 15751, 16007, 16265, 16525, 16788, 17054, 17322, 17592, 17864, 18139,
    18417, 18697, 18979, 19264, 19552, 19842, 20134, 20429, 20727, 21027, 21329, 21634, 21942, 22252, 22564, 22880,
    23198, 23518, 23840, 24166, 24494, 24824, 25158, 25494, 25832, 26173, 26516, 26862, 27211, 27563, 27917, 28273,
    28633, 28995, 29359, 29727, 30097, 30469, 30845, 31223, 31603, 31987, 32373, 32762, 33154, 33548, 33944, 34344,
    34746, 35152, 35560, 35970, 36384, 36800, 37219, 37641, 38065, 38492, 38922, 39355, 39791, 40229, 40670, 41114,
    41561, 42011, 42463, 42919, 43377, 43838, 44302, 44768, 45238, 45710, 46185, 46663, 47144, 47628, 48115, 48605,
    49097, 49593, 50091, 50593, 51097, 51604, 52114, 52627, 53143, 53661, 54183, 54708, 55236, 55766, 56300, 56836,
    57376, 57918, 58464, 59012, 59564, 60118, 60676, 61236, 61800, 62366, 62936, 63508, 64084, 64662, 65244,
};

// THREE COMPILE-TIME PROOFS, because a transcription slip in 511 committed literals is silent and a
// build failure is strictly stronger than a red test.
[[nodiscard]] constexpr bool forwardTableIsStrictlyIncreasing() noexcept {
    for (std::size_t i = 0; i + 1 < SRGB_TO_LINEAR.size(); ++i) {
        if (SRGB_TO_LINEAR[i] >= SRGB_TO_LINEAR[i + 1]) {
            return false;
        }
    }
    return true;
}
[[nodiscard]] constexpr bool thresholdsAreTheMidpoints() noexcept {
    for (std::size_t i = 0; i < LINEAR_TO_SRGB_THRESHOLDS.size(); ++i) {
        const auto midpoint =
            static_cast<std::uint16_t>((static_cast<std::uint32_t>(SRGB_TO_LINEAR[i]) + SRGB_TO_LINEAR[i + 1] + 1) / 2);
        if (LINEAR_TO_SRGB_THRESHOLDS[i] != midpoint) {
            return false;
        }
    }
    return true;
}
static_assert(SRGB_TO_LINEAR.front() == 0 && SRGB_TO_LINEAR.back() == 65535,
              "the sRGB forward table must span the full 16-bit range exactly");
static_assert(forwardTableIsStrictlyIncreasing(),
              "the sRGB forward table must be strictly increasing -- a flat or reversed pair is the "
              "shape a transcription slip produces");
static_assert(thresholdsAreTheMidpoints(),
              "LINEAR_TO_SRGB_THRESHOLDS is DERIVED from SRGB_TO_LINEAR and no longer matches it");

}  // namespace

std::uint32_t mipLevelCount(std::uint32_t width, std::uint32_t height) noexcept {
    const std::uint32_t largest = std::max(width, height);
    if (largest == 0) {
        return 0;
    }
    // An integer shift loop, never std::log2 -- there is no floating point in this subsystem. The
    // loop runs at most 14 times for MAX_TEXTURE_DIMENSION, so no shift can approach a
    // std::uint32_t's width, which would be UB.
    std::uint32_t levels = 1;
    for (std::uint32_t value = largest; value > 1; value >>= 1U) {
        ++levels;
    }
    return levels;
}

std::uint16_t srgbToLinear(std::uint8_t encoded) noexcept { return SRGB_TO_LINEAR[encoded]; }

std::uint8_t linearToSrgb(std::uint16_t linear) noexcept {
    // The number of thresholds <= `linear`, which IS the value whose forward entry is nearest to it.
    // std::upper_bound rather than a hand-rolled loop, and a LOOP rather than recursion (the tree's
    // misc-no-recursion rule).
    const auto position = std::upper_bound(LINEAR_TO_SRGB_THRESHOLDS.begin(), LINEAR_TO_SRGB_THRESHOLDS.end(), linear);
    return static_cast<std::uint8_t>(position - LINEAR_TO_SRGB_THRESHOLDS.begin());
}

namespace {

// One axis of the polyphase box, resolved for destination index `i`.
//
// A 2x2 box is only correct when both dimensions are EVEN. For an odd dimension the naive answers --
// drop the last row/column, or clamp the second tap -- both shift the image by a fraction of a texel
// PER LEVEL, so the shift compounds down the chain and an NPOT texture's small mips visibly walk.
//
//   S even (S == 2D):   two taps,   weights {1, 1},           denominator 2,      indices {2i, 2i+1}
//   S odd  (S == 2D+1): three taps, weights {D-i, D, i+1},    denominator 2D+1,   indices {2i, 2i+1, 2i+2}
//   S == 1 (D == 1):    one tap,    weight  {1},              denominator 1,      index   {0}
//
// The odd weights SUM TO 2D+1 == the denominator for every i, which is the property that makes the
// filter energy-preserving and the reason the naive alternatives shift the image.
struct AxisTaps {
    std::array<std::uint32_t, 3> weight{};
    std::array<std::uint32_t, 3> index{};
    std::uint32_t count = 0;
    std::uint32_t denominator = 1;
};

[[nodiscard]] AxisTaps axisTaps(std::uint32_t sourceExtent, std::uint32_t destinationIndex) noexcept {
    AxisTaps taps;
    if (sourceExtent <= 1) {
        taps.count = 1;
        taps.weight[0] = 1;
        taps.index[0] = 0;
        taps.denominator = 1;
        return taps;
    }
    const std::uint32_t half = sourceExtent >> 1U;
    if ((sourceExtent & 1U) == 0) {
        taps.count = 2;
        taps.weight[0] = 1;
        taps.weight[1] = 1;
        taps.index[0] = 2 * destinationIndex;
        taps.index[1] = 2 * destinationIndex + 1;
        taps.denominator = 2;
        return taps;
    }
    taps.count = 3;
    taps.weight[0] = half - destinationIndex;
    taps.weight[1] = half;
    taps.weight[2] = destinationIndex + 1;
    taps.index[0] = 2 * destinationIndex;
    taps.index[1] = 2 * destinationIndex + 1;
    taps.index[2] = 2 * destinationIndex + 2;
    taps.denominator = 2 * half + 1;
    return taps;
}

}  // namespace

void downsampleRgba8(std::span<const std::byte> src, std::uint32_t srcWidth, std::uint32_t srcHeight,
                     std::span<std::byte> dst, bool srgb) noexcept {
    AERO_PROFILE_ZONE_NAMED("assets::downsampleRgba8");

    if (srcWidth == 0 || srcHeight == 0) {
        return;
    }
    const std::uint32_t dstWidth = std::max(1U, srcWidth >> 1U);
    const std::uint32_t dstHeight = std::max(1U, srcHeight >> 1U);
    const std::uint64_t sourceBytes = static_cast<std::uint64_t>(srcWidth) * srcHeight * 4;
    const std::uint64_t destinationBytes = static_cast<std::uint64_t>(dstWidth) * dstHeight * 4;
    // A size mismatch writes NOTHING: a caller bug must not become a read or a partial result.
    if (src.size() != sourceBytes || dst.size() != destinationBytes) {
        return;
    }

    for (std::uint32_t y = 0; y < dstHeight; ++y) {
        const AxisTaps rows = axisTaps(srcHeight, y);
        for (std::uint32_t x = 0; x < dstWidth; ++x) {
            const AxisTaps columns = axisTaps(srcWidth, x);
            const std::uint64_t denominator = static_cast<std::uint64_t>(columns.denominator) * rows.denominator;

            // ONE fused 2D weighted sum with ONE rounding step, never two separable passes -- two
            // passes round twice and the second rounding is applied to already-rounded values.
            //
            // The accumulator is bounded by denominator * 65535, i.e. at most
            // 16383 * 16383 * 65535 ~= 1.76e13, which fits a std::uint64_t with four orders of
            // magnitude to spare. THE WIDTH IS FORCED AT THE MULTIPLICATION, never after: written as
            // `weight * weight * sample` the product promotes to int and 16385 * 16385 * 65535
            // overflows a signed 32-bit int, which is UB that no committed fixture is large enough
            // to trigger.
            std::array<std::uint64_t, 4> accumulator{};
            for (std::uint32_t ty = 0; ty < rows.count; ++ty) {
                for (std::uint32_t tx = 0; tx < columns.count; ++tx) {
                    const std::uint64_t weight = static_cast<std::uint64_t>(rows.weight[ty]) * columns.weight[tx];
                    const std::size_t texel =
                        (static_cast<std::size_t>(rows.index[ty]) * srcWidth + columns.index[tx]) * 4;
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const auto stored = static_cast<std::uint8_t>(src[texel + channel]);
                        // sRGB decodes to LINEAR LIGHT before the sum; a *Unorm format's stored
                        // values already ARE linear and are summed as they stand.
                        accumulator[channel] += weight * (srgb ? srgbToLinear(stored) : stored);
                    }
                    // ALPHA IS SUMMED AS STORED IN BOTH CASES. Alpha is coverage, never a
                    // gamma-encoded colour, and gamma-correcting it is a real and subtle bug.
                    accumulator[3] += weight * static_cast<std::uint8_t>(src[texel + 3]);
                }
            }

            const std::size_t out = (static_cast<std::size_t>(y) * dstWidth + x) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                // Round half UP, integer.
                const std::uint64_t averaged = (accumulator[channel] + denominator / 2) / denominator;
                dst[out + channel] = static_cast<std::byte>(srgb ? linearToSrgb(static_cast<std::uint16_t>(averaged))
                                                                 : static_cast<std::uint8_t>(averaged));
            }
            const std::uint64_t alpha = (accumulator[3] + denominator / 2) / denominator;
            dst[out + 3] = static_cast<std::byte>(static_cast<std::uint8_t>(alpha));
        }
    }
}

}  // namespace engine::assets::detail

namespace engine::assets {
namespace {

// The header's field offsets, named ONCE for the writer, exactly as cooked_texture.cpp names them for
// the parser. Nothing else in this TU spells a header offset as a literal.
constexpr std::size_t H_VK_FORMAT = 12;
constexpr std::size_t H_TYPE_SIZE = 16;
constexpr std::size_t H_PIXEL_WIDTH = 20;
constexpr std::size_t H_PIXEL_HEIGHT = 24;
constexpr std::size_t H_FACE_COUNT = 36;
constexpr std::size_t H_LEVEL_COUNT = 40;
static_assert(H_FACE_COUNT + 4 == H_LEVEL_COUNT);  // ... with pixelDepth and layerCount before it
constexpr std::size_t H_DFD_OFFSET = 48;
constexpr std::size_t H_DFD_LENGTH = 52;
constexpr std::size_t H_KVD_OFFSET = 56;
constexpr std::size_t H_KVD_LENGTH = 60;
static_assert(H_KVD_LENGTH + 4 + 8 + 8 == KTX2_HEADER_BYTES);  // ... then the two u64 sgd fields

constexpr std::size_t L_BYTE_OFFSET = 0;
constexpr std::size_t L_BYTE_LENGTH = 8;
constexpr std::size_t L_UNCOMPRESSED_LENGTH = 16;
static_assert(L_UNCOMPRESSED_LENGTH + 8 == KTX2_LEVEL_RECORD_BYTES);

constexpr std::string_view KVD_KEY_SOURCE_GUID = "AeroSourceGuid";
constexpr std::string_view KVD_KEY_ORIENTATION = "KTXorientation";
constexpr std::string_view KVD_VALUE_ORIENTATION = "rd";  // right-and-down: a TOP-LEFT origin
constexpr std::string_view KVD_KEY_WRITER = "KTXwriter";

[[nodiscard]] constexpr std::uint32_t levelExtent(std::uint32_t base, std::uint32_t level) noexcept {
    return std::max(1U, base >> level);
}

// The same arithmetic cooked_texture.cpp's parser computes independently. The duplication is
// deliberate and it is COVERED: CT45-CT52 parse the cook's own output for all eight formats, so a
// divergence between the two is a red test rather than a file this tree's own reader refuses.
[[nodiscard]] constexpr std::uint64_t levelByteLength(CookedTextureFormat format, std::uint32_t width,
                                                      std::uint32_t height, std::uint32_t level) noexcept {
    const std::uint32_t blockWidth = cookedTextureBlockWidth(format);
    const std::uint32_t blockHeight = cookedTextureBlockHeight(format);
    const std::uint64_t blocksX = (levelExtent(width, level) + blockWidth - 1) / blockWidth;
    const std::uint64_t blocksY = (levelExtent(height, level) + blockHeight - 1) / blockHeight;
    return blocksX * blocksY * cookedTextureBlockBytes(format);
}

[[nodiscard]] TextureCookResult refuse(std::string message) {
    TextureCookResult out;
    out.status = TextureCookStatus::Refused;
    out.message = std::move(message);
    return out;  // `bytes` stays EMPTY, which is the status's other half
}

struct KvdRecord {
    std::string_view key;
    std::string_view value;
};

// Gathers a 4x4 footprint out of `source` with the sample coordinate CLAMPED to the level's extent.
// REPLICATION, NEVER ZERO-FILL: zero-fill drags the block's endpoints toward black and visibly darkens
// the image's right and bottom edges. This is the caller's job precisely so the encoders always see
// sixteen valid texels.
[[nodiscard]] std::array<std::uint8_t, 64> gatherBlock(std::span<const std::byte> source, std::uint32_t width,
                                                       std::uint32_t height, std::uint32_t blockX,
                                                       std::uint32_t blockY) noexcept {
    std::array<std::uint8_t, 64> texels{};
    for (std::uint32_t ty = 0; ty < 4; ++ty) {
        const std::uint32_t sy = std::min(blockY * 4 + ty, height - 1);
        for (std::uint32_t tx = 0; tx < 4; ++tx) {
            const std::uint32_t sx = std::min(blockX * 4 + tx, width - 1);
            const std::size_t from = (static_cast<std::size_t>(sy) * width + sx) * 4;
            const std::size_t to = static_cast<std::size_t>(4 * ty + tx) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                texels[to + channel] = static_cast<std::uint8_t>(source[from + channel]);
            }
        }
    }
    return texels;
}

[[nodiscard]] std::array<std::uint8_t, 16> channelOf(const std::array<std::uint8_t, 64>& texels,
                                                     std::size_t channel) noexcept {
    std::array<std::uint8_t, 16> out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[i] = texels[4 * i + channel];
    }
    return out;
}

// Encodes one level of `source` into `artifact` at `at`. The block order within a level is ROW-MAJOR
// OVER BLOCKS -- `by` outer, `bx` inner -- which is what every BCn consumer expects and what the
// level's byteLength arithmetic assumes.
void encodeLevel(std::span<const std::byte> source, std::uint32_t width, std::uint32_t height,
                 CookedTextureFormat format, std::span<std::byte> artifact, std::size_t at) noexcept {
    if (format == CookedTextureFormat::Rgba8Unorm || format == CookedTextureFormat::Rgba8Srgb) {
        // VERBATIM. No conversion of any kind, which is what makes level 0 of an Rgba8* cook
        // bit-identical to its input -- the colour space changes the vkFormat and the descriptor,
        // never the bytes.
        for (std::size_t i = 0; i < source.size(); ++i) {
            artifact[at + i] = source[i];
        }
        return;
    }
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    const std::uint32_t blockBytes = cookedTextureBlockBytes(format);
    for (std::uint32_t by = 0; by < blocksY; ++by) {
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            const std::array<std::uint8_t, 64> texels = gatherBlock(source, width, height, bx, by);
            const std::size_t block = at + (static_cast<std::size_t>(by) * blocksX + bx) * blockBytes;
            switch (format) {
                case CookedTextureFormat::Bc1RgbUnorm:
                case CookedTextureFormat::Bc1RgbSrgb:
                    encodeBc1Block(texels, std::span<std::byte, 8>(artifact.data() + block, 8));
                    break;
                case CookedTextureFormat::Bc3Unorm:
                case CookedTextureFormat::Bc3Srgb:
                    // ALPHA FIRST, then colour. An output-byte decision: swapping the two produces a
                    // plausible image rather than an obviously broken one.
                    encodeBc4Block(channelOf(texels, 3), std::span<std::byte, 8>(artifact.data() + block, 8));
                    encodeBc1Block(texels, std::span<std::byte, 8>(artifact.data() + block + 8, 8));
                    break;
                case CookedTextureFormat::Bc4Unorm:
                    encodeBc4Block(channelOf(texels, 0), std::span<std::byte, 8>(artifact.data() + block, 8));
                    break;
                case CookedTextureFormat::Bc5Unorm:
                    // RED FIRST, then green. The same kind of decision as BC3's, with the same
                    // failure mode.
                    encodeBc4Block(channelOf(texels, 0), std::span<std::byte, 8>(artifact.data() + block, 8));
                    encodeBc4Block(channelOf(texels, 1), std::span<std::byte, 8>(artifact.data() + block + 8, 8));
                    break;
                case CookedTextureFormat::Rgba8Unorm:
                case CookedTextureFormat::Rgba8Srgb:
                    break;  // handled above; listed so a new enumerator is a -Wswitch error
            }
        }
    }
}

}  // namespace

TextureCookResult cookTexture(const TextureCookInput& input) {
    AERO_PROFILE_ZONE_NAMED("assets::cookTexture");

    // 1. the FORMAT, before anything is derived from it. CookedTextureFormat has a fixed underlying
    //    type, so every std::uint32_t is a valid value of it and static_cast<CookedTextureFormat>(42)
    //    is well-formed -- an enumerator outside the eight is something this function can genuinely be
    //    handed the moment a format is read from a .meta, a settings file or a .pak. For such a value
    //    cookedTextureBlockBytes answers 0, std::lcm(0, 4) is 0, and the level-data alignment below
    //    would DIVIDE BY ZERO. parseCookedTexture gates the identical question with the identical
    //    predicate (cooked_texture.cpp step 5); this is the producer's own half of that gate, and it
    //    comes first because every step after it computes something from the format.
    if (!isCookedTextureFormat(static_cast<std::uint32_t>(input.format))) {
        return refuse(std::format("vkFormat {} is outside this cook's eight-format subset",
                                  static_cast<std::uint32_t>(input.format)));
    }

    // 2. dimensions, before anything is computed from them.
    if (input.width == 0 || input.width > MAX_TEXTURE_DIMENSION) {
        return refuse(std::format("width is {}; the legal range is 1..{}", input.width, MAX_TEXTURE_DIMENSION));
    }
    if (input.height == 0 || input.height > MAX_TEXTURE_DIMENSION) {
        return refuse(std::format("height is {}; the legal range is 1..{}", input.height, MAX_TEXTURE_DIMENSION));
    }

    // 3. the input span's size must match the dimensions EXACTLY. Computed in u64: at the dimension
    //    cap this is 1 073 741 824, well inside range.
    const std::uint64_t sourceByteSize = static_cast<std::uint64_t>(input.width) * input.height * 4;
    if (input.rgba8.size() != sourceByteSize) {
        return refuse(std::format("the pixel span is {} bytes but {}x{} RGBA8 is {} bytes", input.rgba8.size(),
                                  input.width, input.height, sourceByteSize));
    }

    // 4. the level count. Complete chain or exactly one -- KTX2 permits an incomplete pyramid and this
    //    container refuses it, because a partial chain's only effect is to make a consumer's sampler
    //    configuration depend on the file.
    const std::uint32_t levelCount = input.generateMips ? detail::mipLevelCount(input.width, input.height) : 1;
    // ASSERTED, not assumed: it holds by construction of MAX_TEXTURE_DIMENSION, and this is what makes
    // a future cap change a build-time conversation rather than a silently over-long level index.
    if (levelCount == 0 || levelCount > MAX_TEXTURE_LEVELS) {
        return refuse(std::format("the computed level count is {}, outside 1..{}", levelCount, MAX_TEXTURE_LEVELS));
    }

    // 5. the whole layout, computed in u64 BEFORE a single byte is allocated.
    const CookedTextureFormat format = input.format;
    const std::span<const std::uint8_t> descriptor = cookedTextureDescriptorBytes(format);
    const std::uint32_t alignment = cookedTextureLevelAlignment(format);
    const std::uint64_t levelIndexBytes = static_cast<std::uint64_t>(levelCount) * KTX2_LEVEL_RECORD_BYTES;
    const std::uint64_t dfdOffset = KTX2_HEADER_BYTES + levelIndexBytes;
    const std::uint64_t kvdOffset = dfdOffset + descriptor.size();
    const std::uint64_t kvdEnd = kvdOffset + KTX2_KVD_BYTES;
    // THE ONE PADDING SITE IN THE WHOLE FILE. Every level's byteLength is a multiple of the format's
    // own alignment, so once the FIRST WRITTEN level (the smallest) is aligned every subsequent level
    // start is aligned automatically. The parser checks every level anyway, because a hostile file is
    // not obliged to share our arithmetic.
    const std::uint64_t levelDataStart = ((kvdEnd + alignment - 1) / alignment) * alignment;

    // Level DATA is written smallest-first while the level INDEX is filled level-0-first, so the
    // offsets are computed in REVERSE and stored FORWARD. That inversion is the single most likely
    // place for an off-by-one in this whole task, and CT56 plus the byte goldens exist to pin it.
    std::array<std::uint64_t, MAX_TEXTURE_LEVELS> levelOffset{};
    std::array<std::uint64_t, MAX_TEXTURE_LEVELS> levelBytes{};
    std::uint64_t cursor = levelDataStart;
    std::uint64_t blockCount = 0;
    for (std::uint32_t level = levelCount; level-- > 0;) {
        levelOffset[level] = cursor;
        levelBytes[level] = levelByteLength(format, input.width, input.height, level);
        blockCount += levelBytes[level] / cookedTextureBlockBytes(format);
        cursor += levelBytes[level];
    }
    const std::uint64_t totalBytes = cursor;

    // THE BYTE-CAP CHECK COMES BEFORE THE ALLOCATION IT BOUNDS, and that ordering is asserted in
    // comment-stripped SOURCE TEXT by TX48 rather than by a runtime case -- a 512 MB reserve of
    // virtual address space simply succeeds on a 64-bit host, so nothing at runtime can see this
    // violated (task 3.3.1 measured exactly that at 274 GB).
    if (totalBytes > MAX_COOKED_TEXTURE_BYTES) {
        return refuse(std::format("the cooked artifact would be {} bytes, over the {}-byte cap", totalBytes,
                                  MAX_COOKED_TEXTURE_BYTES));
    }
    // ONE zero-initialized buffer of the pre-computed total size, written field by field through
    // cooked_mesh.hpp's primitives. ALL PADDING IS THEREFORE ZERO WITHOUT A SINGLE EXPLICIT PAD WRITE.
    std::vector<std::byte> artifact(static_cast<std::size_t>(totalBytes), std::byte{0});
    const std::span<std::byte> out(artifact);

    for (std::size_t i = 0; i < KTX2_IDENTIFIER.size(); ++i) {
        out[i] = static_cast<std::byte>(KTX2_IDENTIFIER[i]);
    }
    putU32(out, H_VK_FORMAT, static_cast<std::uint32_t>(format));
    putU32(out, H_TYPE_SIZE, 1);
    putU32(out, H_PIXEL_WIDTH, input.width);
    putU32(out, H_PIXEL_HEIGHT, input.height);
    // pixelDepth, layerCount, supercompressionScheme and the two sgd u64s are all ZERO, which the
    // zero-initialized buffer already gives; faceCount is the only shape field that is not.
    putU32(out, H_FACE_COUNT, 1);
    putU32(out, H_LEVEL_COUNT, levelCount);
    putU32(out, H_DFD_OFFSET, static_cast<std::uint32_t>(dfdOffset));
    putU32(out, H_DFD_LENGTH, static_cast<std::uint32_t>(descriptor.size()));
    putU32(out, H_KVD_OFFSET, static_cast<std::uint32_t>(kvdOffset));
    putU32(out, H_KVD_LENGTH, KTX2_KVD_BYTES);

    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const std::size_t record = KTX2_HEADER_BYTES + static_cast<std::size_t>(level) * KTX2_LEVEL_RECORD_BYTES;
        putU64(out, record + L_BYTE_OFFSET, levelOffset[level]);
        putU64(out, record + L_BYTE_LENGTH, levelBytes[level]);
        // uncompressedByteLength == byteLength, since supercompressionScheme is 0.
        putU64(out, record + L_UNCOMPRESSED_LENGTH, levelBytes[level]);
    }

    for (std::size_t i = 0; i < descriptor.size(); ++i) {
        out[static_cast<std::size_t>(dfdOffset) + i] = static_cast<std::byte>(descriptor[i]);
    }

    // The three key/value records, CONSTRUCTED IN A DELIBERATELY WRONG ORDER AND THEN SORTED, so the
    // sort is load-bearing: with the records already in sorted order a seed that removed the sort
    // entirely would change nothing, and the spec's "sorted by Unicode code point" would have no case
    // that could see it violated. Every key here is ASCII, so a byte-wise compare IS a code-point
    // compare and `char`'s signedness cannot matter.
    //
    // AeroSourceGuid is written UNCONDITIONALLY, including for the nil GUID (32 '0' characters), so
    // the layout never depends on whether a GUID was supplied. Keys beginning KTX/ktx are reserved by
    // the spec; AeroSourceGuid is not.
    const std::string guidText = formatGuid(input.sourceGuid);
    std::array<KvdRecord, 3> records = {
        KvdRecord{KVD_KEY_WRITER, COOKED_TEXTURE_WRITER_ID},
        KvdRecord{KVD_KEY_SOURCE_GUID, guidText},
        KvdRecord{KVD_KEY_ORIENTATION, KVD_VALUE_ORIENTATION},
    };
    std::sort(records.begin(), records.end(), [](const KvdRecord& a, const KvdRecord& b) { return a.key < b.key; });

    auto kvdCursor = static_cast<std::size_t>(kvdOffset);
    for (const KvdRecord& record : records) {
        const std::size_t keyAndValueByteLength = record.key.size() + 1 + record.value.size() + 1;
        putU32(out, kvdCursor, static_cast<std::uint32_t>(keyAndValueByteLength));
        std::size_t at = kvdCursor + 4;
        for (const char c : record.key) {
            out[at++] = static_cast<std::byte>(c);
        }
        ++at;  // the key's NUL is already zero
        for (const char c : record.value) {
            out[at++] = static_cast<std::byte>(c);
        }
        // The value's NUL and the record's valuePadding are already zero -- see the buffer above.
        kvdCursor += 4 + detail::align4(keyAndValueByteLength);
    }

    // The mip chain, with at most TWO levels resident at once. Level 0 is the caller's own span and is
    // never copied; levels 1 and up alternate between two buffers, so the peak on top of the caller's
    // image is level 1's RGBA8 plus level 2's -- for a 4096^2 BC3 cook, about 16.7 MB + 4.2 MB
    // alongside the 22 MB output vector. A vector of ALL levels would instead hold 4/3 x the base in
    // RGBA8 and turn a 179 MB BC1 cook into a 1.4 GB resident peak.
    const bool srgb = isSrgbCookedFormat(format);
    std::span<const std::byte> currentLevel = input.rgba8;
    std::uint32_t currentWidth = input.width;
    std::uint32_t currentHeight = input.height;
    std::array<std::vector<std::byte>, 2> scratch;
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        if (level > 0) {
            const std::uint32_t nextWidth = levelExtent(input.width, level);
            const std::uint32_t nextHeight = levelExtent(input.height, level);
            std::vector<std::byte>& target = scratch[level % 2];
            target.assign(static_cast<std::size_t>(nextWidth) * nextHeight * 4, std::byte{0});
            // Level p is filtered FROM LEVEL p-1, never resampled from level 0: resampling from the
            // base would make each level a different filter's output and would cost O(levels x base).
            detail::downsampleRgba8(currentLevel, currentWidth, currentHeight, target, srgb);
            currentLevel = target;
            currentWidth = nextWidth;
            currentHeight = nextHeight;
        }
        encodeLevel(currentLevel, currentWidth, currentHeight, format, out,
                    static_cast<std::size_t>(levelOffset[level]));
    }

    TextureCookResult result;
    result.bytes = std::move(artifact);
    result.stats.levelCount = levelCount;
    result.stats.blockCount = static_cast<std::uint32_t>(blockCount);
    result.stats.byteSize = totalBytes;
    result.stats.sourceByteSize = sourceByteSize;
    return result;
}

}  // namespace engine::assets
