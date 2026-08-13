// Aero Engine — the texture cook (task 3.3.2): the committed gamma tables, the integer polyphase mip
// filter and (from step 5) cookTexture itself. See texture_cook.hpp for the contract and
// docs/09-file-formats.md section 10.8 for the normative filter and table definitions.
//
// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. NO FLOATING POINT.
#include <aero/assets/texture_cook.hpp>
#include <aero/core/profiler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

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
