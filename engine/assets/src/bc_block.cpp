// Aero Engine — the BC1 and BC4 block encoders (task 3.3.2). See bc_block.hpp for the contract and
// docs/09-file-formats.md section 10.9 for the normative algorithm -- these are OUTPUT-BYTE decisions,
// so they belong in the normative document and not only in a header.
//
// INTEGER ONLY. There is no float, no double, no <cmath> and no runtime table in this file, and that
// is the property the whole first-party-encoder decision exists to buy.
#include <aero/assets/bc_block.hpp>
#include <aero/assets/cooked_mesh.hpp>  // putU16 / putU32 -- the only place bytes are formed here

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace engine::assets {
namespace {

constexpr std::size_t TEXELS_PER_BLOCK = 16;

struct Rgb {
    std::int32_t r = 0;
    std::int32_t g = 0;
    std::int32_t b = 0;
};

[[nodiscard]] constexpr std::int32_t clampByte(std::int64_t v) noexcept {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(v, 0, 255));
}

// Floor division for a POSITIVE divisor. C++ integer division truncates toward zero, so the negative
// branch is written out rather than assumed -- a least-squares endpoint can legitimately come out
// below zero before it is clamped, and "round toward zero" and "round toward -infinity" disagree
// there, which would be an output-byte difference hiding in a sign.
[[nodiscard]] constexpr std::int64_t floorDiv(std::int64_t numerator, std::int64_t denominator) noexcept {
    const std::int64_t quotient = numerator / denominator;
    return (numerator % denominator != 0 && numerator < 0) ? quotient - 1 : quotient;
}

// floor(numerator / denominator + 1/2), i.e. round half UP (toward +infinity), for a positive
// denominator. One rule, no sign-dependent rounding.
[[nodiscard]] constexpr std::int64_t roundHalfUp(std::int64_t numerator, std::int64_t denominator) noexcept {
    return floorDiv(2 * numerator + denominator, 2 * denominator);
}

// ---- BC1 -----------------------------------------------------------------------------------------

// RGB565, round to nearest. There is never an exact half here: v * 31 == 255k + 127.5 has no integer
// solution, and neither does the 63 form, so `+ 127` and `+ 128` agree and the tie rule is moot.
[[nodiscard]] constexpr std::uint32_t quantize565(Rgb colour) noexcept {
    const auto r5 = static_cast<std::uint32_t>((colour.r * 31 + 127) / 255);
    const auto g6 = static_cast<std::uint32_t>((colour.g * 63 + 127) / 255);
    const auto b5 = static_cast<std::uint32_t>((colour.b * 31 + 127) / 255);
    return (r5 << 11U) | (g6 << 5U) | b5;
}

// BIT REPLICATION, not `(v * 255 + 15) / 31`. This is what a hardware decoder does, and the palette
// must be built from the values a DECODER will produce or every interpolated colour is subtly off.
[[nodiscard]] constexpr Rgb dequantize565(std::uint32_t packed) noexcept {
    const auto r5 = static_cast<std::int32_t>((packed >> 11U) & 0x1FU);
    const auto g6 = static_cast<std::int32_t>((packed >> 5U) & 0x3FU);
    const auto b5 = static_cast<std::int32_t>(packed & 0x1FU);
    return Rgb{(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)};
}

// The four-colour palette, computed on the DEQUANTIZED endpoints. The +1 before the /3 is the
// rounding this format freezes; the reference decoder in tests/bc_block_test.cpp reproduces it from
// the same definition rather than by calling in here.
[[nodiscard]] constexpr std::array<Rgb, 4> buildBc1Palette(Rgb a, Rgb b) noexcept {
    return {a, b, Rgb{(2 * a.r + b.r + 1) / 3, (2 * a.g + b.g + 1) / 3, (2 * a.b + b.b + 1) / 3},
            Rgb{(a.r + 2 * b.r + 1) / 3, (a.g + 2 * b.g + 1) / 3, (a.b + 2 * b.b + 1) / 3}};
}

// Ties take the LOWER index, which is what the strict `<` gives. Stated because it is an output-byte
// decision, not an implementation detail.
[[nodiscard]] constexpr std::uint32_t nearestBc1Index(const std::array<Rgb, 4>& palette, Rgb texel) noexcept {
    std::uint32_t best = 0;
    std::int32_t bestError = std::numeric_limits<std::int32_t>::max();
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::int32_t dr = palette[i].r - texel.r;
        const std::int32_t dg = palette[i].g - texel.g;
        const std::int32_t db = palette[i].b - texel.b;
        // At most 10 * 255^2 == 650250, comfortably inside a std::int32_t.
        const std::int32_t error =
            BC1_ERROR_WEIGHT_R * dr * dr + BC1_ERROR_WEIGHT_G * dg * dg + BC1_ERROR_WEIGHT_B * db * db;
        if (error < bestError) {
            bestError = error;
            best = i;
        }
    }
    return best;
}

// A palette index expressed as its interpolation weight over {0, 1/3, 2/3, 1}, scaled by 3 so the
// whole least-squares solve stays in integers: index 0 is endpoint a (weight 0), index 1 is endpoint
// b (weight 3), index 2 is (2a+b)/3 (weight 1) and index 3 is (a+2b)/3 (weight 2).
constexpr std::array<std::int64_t, 4> BC1_INDEX_WEIGHT = {0, 3, 1, 2};

// The integer least-squares refit. colour(w) = ((3-w) * a + w * b) / 3, so the normal equations are
//   (sum p^2) a + (sum p q) b = 3 * sum p x        with p = 3 - w
//   (sum p q) a + (sum q^2) b = 3 * sum q x             q = w
// solved by Cramer's rule. Every intermediate is bounded: the sums of squares by 144, the weighted
// channel sums by 16 * 3 * 255 == 12240, so the largest numerator is 3 * 12240 * 144 == 5 287 680.
//
// Returns false when the system is DEGENERATE (every texel took the same index, so the determinant is
// zero). The caller then KEEPS the current endpoints -- it never divides.
[[nodiscard]] bool fitBc1Endpoints(const std::array<Rgb, TEXELS_PER_BLOCK>& texels,
                                   const std::array<std::uint32_t, TEXELS_PER_BLOCK>& indices, Rgb& a,
                                   Rgb& b) noexcept {
    std::int64_t sumPp = 0;
    std::int64_t sumPq = 0;
    std::int64_t sumQq = 0;
    std::array<std::int64_t, 3> sumPx{};
    std::array<std::int64_t, 3> sumQx{};
    for (std::size_t i = 0; i < TEXELS_PER_BLOCK; ++i) {
        const std::int64_t q = BC1_INDEX_WEIGHT[indices[i]];
        const std::int64_t p = 3 - q;
        sumPp += p * p;
        sumPq += p * q;
        sumQq += q * q;
        const std::array<std::int64_t, 3> channels = {texels[i].r, texels[i].g, texels[i].b};
        for (std::size_t c = 0; c < 3; ++c) {
            sumPx[c] += p * channels[c];
            sumQx[c] += q * channels[c];
        }
    }
    const std::int64_t determinant = sumPp * sumQq - sumPq * sumPq;
    if (determinant == 0) {
        return false;
    }
    std::array<std::int32_t, 3> fitA{};
    std::array<std::int32_t, 3> fitB{};
    for (std::size_t c = 0; c < 3; ++c) {
        fitA[c] = clampByte(roundHalfUp(3 * sumPx[c] * sumQq - 3 * sumQx[c] * sumPq, determinant));
        fitB[c] = clampByte(roundHalfUp(3 * sumQx[c] * sumPp - 3 * sumPx[c] * sumPq, determinant));
    }
    a = Rgb{fitA[0], fitA[1], fitA[2]};
    b = Rgb{fitB[0], fitB[1], fitB[2]};
    return true;
}

void assignBc1Indices(const std::array<Rgb, TEXELS_PER_BLOCK>& texels, std::uint32_t c0, std::uint32_t c1,
                      std::array<std::uint32_t, TEXELS_PER_BLOCK>& indices) noexcept {
    const std::array<Rgb, 4> palette = buildBc1Palette(dequantize565(c0), dequantize565(c1));
    for (std::size_t i = 0; i < TEXELS_PER_BLOCK; ++i) {
        indices[i] = nearestBc1Index(palette, texels[i]);
    }
}

// ---- BC4 -----------------------------------------------------------------------------------------

// The eight-value palette: r0, r1, then the six interpolants. The +3 before the /7 is this format's
// frozen rounding, exactly like BC1's +1 before the /3.
[[nodiscard]] constexpr std::array<std::int32_t, 8> buildBc4Palette(std::int32_t r0, std::int32_t r1) noexcept {
    std::array<std::int32_t, 8> palette{};
    palette[0] = r0;
    palette[1] = r1;
    for (std::int32_t k = 1; k <= 6; ++k) {
        palette[static_cast<std::size_t>(k) + 1] = ((7 - k) * r0 + k * r1 + 3) / 7;
    }
    return palette;
}

}  // namespace

void encodeBc1Block(std::span<const std::uint8_t, 64> srcRgba, std::span<std::byte, 8> dst) noexcept {
    std::array<Rgb, TEXELS_PER_BLOCK> texels{};
    for (std::size_t i = 0; i < TEXELS_PER_BLOCK; ++i) {
        // Alpha (srcRgba[4 * i + 3]) is READ BY NOBODY: VK_FORMAT_BC1_RGB_* carries none.
        texels[i] = Rgb{srcRgba[4 * i + 0], srcRgba[4 * i + 1], srcRgba[4 * i + 2]};
    }

    // 1. The bounding box's two corners are the initial endpoints. The LOW corner goes first, so the
    //    four-colour ordering swap below is exercised by every non-degenerate block rather than by a
    //    rare one -- a rule with no case that can see it violated is a rule with no cover.
    Rgb lo = texels[0];
    Rgb hi = texels[0];
    for (const Rgb& texel : texels) {
        lo = Rgb{std::min(lo.r, texel.r), std::min(lo.g, texel.g), std::min(lo.b, texel.b)};
        hi = Rgb{std::max(hi.r, texel.r), std::max(hi.g, texel.g), std::max(hi.b, texel.b)};
    }

    // 2. Quantize to RGB565, build the palette, assign.
    std::uint32_t c0 = quantize565(lo);
    std::uint32_t c1 = quantize565(hi);
    std::array<std::uint32_t, TEXELS_PER_BLOCK> indices{};
    assignBc1Indices(texels, c0, c1, indices);

    // 3. Refit, EXACTLY twice.
    for (std::int32_t iteration = 0; iteration < BC1_REFINEMENT_ITERATIONS; ++iteration) {
        Rgb a{};
        Rgb b{};
        if (!fitBc1Endpoints(texels, indices, a, b)) {
            continue;  // degenerate system: KEEP the current endpoints, never divide
        }
        c0 = quantize565(a);
        c1 = quantize565(b);
        assignBc1Indices(texels, c0, c1, indices);
    }

    // 4. Order. The block must be FOUR-COLOUR, which the format spells as c0 > c1. Swapping the
    //    endpoints maps palette entry 0 to 1 and 2 to 3 and back -- (2a+b)/3 becomes (b+2a)/3, the
    //    same colour under the other index -- so remapping every index with `^ 1` is exact.
    if (c0 < c1) {
        std::swap(c0, c1);
        for (std::uint32_t& index : indices) {
            index ^= 1U;
        }
    }

    // 5. THE ONE DEGENERATE CASE. Equal quantized endpoints put the block in three-colour mode, where
    //    index 0 still decodes to exactly c0. For VK_FORMAT_BC1_RGB_* the fourth entry's alpha is
    //    ignored, so nothing is lost, and this is the only case where the emitted block is not
    //    four-colour.
    if (c0 == c1) {
        indices.fill(0);
    }

    std::uint32_t packed = 0;
    for (std::size_t i = 0; i < TEXELS_PER_BLOCK; ++i) {
        // Texel (x, y) sits at bit 2 * (4 * y + x) of a 32-bit little-endian word at byte 4.
        packed |= indices[i] << (2U * static_cast<std::uint32_t>(i));
    }
    putU16(dst, 0, static_cast<std::uint16_t>(c0));
    putU16(dst, 2, static_cast<std::uint16_t>(c1));
    putU32(dst, 4, packed);
}

void encodeBc4Block(std::span<const std::uint8_t, 16> src, std::span<std::byte, 8> dst) noexcept {
    std::int32_t r0 = src[0];
    std::int32_t r1 = src[0];
    for (const std::uint8_t value : src) {
        r0 = std::max(r0, static_cast<std::int32_t>(value));
        r1 = std::min(r1, static_cast<std::int32_t>(value));
    }

    // r0 == max and r1 == min, which selects the EIGHT-VALUE mode (the format spells it r0 > r1).
    // When they are equal the block is a constant: six-value mode, where index 0 still decodes to
    // exactly r0, so all-zero indices reproduce it exactly. Same shape as BC1's degenerate arm.
    const std::array<std::int32_t, 8> palette = buildBc4Palette(r0, r1);
    std::array<std::uint32_t, TEXELS_PER_BLOCK> indices{};
    if (r0 != r1) {
        for (std::size_t i = 0; i < TEXELS_PER_BLOCK; ++i) {
            std::uint32_t best = 0;
            std::int32_t bestError = std::numeric_limits<std::int32_t>::max();
            for (std::uint32_t p = 0; p < 8; ++p) {
                // Exact integer distance, with the absolute value written out rather than taken
                // through std::abs -- whose integer overload lives in <cstdlib> and whose floating
                // overloads live in <cmath>, and <cmath> may never be included in this subsystem.
                // Ties take the LOWER index, which the strict `<` gives.
                const std::int32_t difference = palette[p] - static_cast<std::int32_t>(src[i]);
                const std::int32_t error = difference < 0 ? -difference : difference;
                if (error < bestError) {
                    bestError = error;
                    best = p;
                }
            }
            indices[i] = best;
        }
    }

    std::uint64_t packed = 0;
    for (std::size_t i = 0; i < TEXELS_PER_BLOCK; ++i) {
        // Texel (x, y) sits at bit 3 * (4 * y + x) of a 48-bit little-endian field at byte 2, so the
        // top index ends at bit 47. Written as a u32 at offset 2 plus a u16 at offset 6 -- NEVER a
        // putU64 at offset 2, which would write two bytes past the block.
        packed |= static_cast<std::uint64_t>(indices[i]) << (3U * static_cast<std::uint64_t>(i));
    }
    // r0 at byte 0 and r1 at byte 1, assembled as ONE little-endian u16 through putU16 rather than as
    // two direct stores, so every byte in this subsystem still comes out of the eight primitives.
    const auto endpoints =
        static_cast<std::uint16_t>(static_cast<std::uint32_t>(r0) | (static_cast<std::uint32_t>(r1) << 8U));
    putU16(dst, 0, endpoints);
    putU32(dst, 2, static_cast<std::uint32_t>(packed & 0xFFFFFFFFU));
    putU16(dst, 6, static_cast<std::uint16_t>((packed >> 32U) & 0xFFFFU));
}

}  // namespace engine::assets
