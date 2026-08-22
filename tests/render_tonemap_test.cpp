// tests/render_tonemap_test.cpp -- task 3.6.3: the tonemap/gamma pass (TM1-TM29, PP1-PP13). A TU of
// aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier 0 (no GPU, every configuration, TM*): the transfer chain, the sanitizer's totality, the sRGB
// OETF tied back to the table 3.3.2 verified against the spec and against `ktx validate`, the UV
// sub-rect rule, the two uniform packers pinned against BYTE LITERALS, and the comment-stripped
// source-text pin tying shaders/tonemap.frag.hlsl and shaders/fullscreen.vert.hlsl to
// engine/render/tonemap.hpp.
//
// Tier 1 (a real Device, NO window -- RenderTarget supplies the formats, gated by AERO_SKIP_OR_FAIL,
// PP*): PostProcess's cycle, both latches, the move/refusal asymmetry and the two-instance case.
//
// tonemap_pack.hpp is PRIVATE to engine/render (src/, never installed), so it is reached by a
// relative include -- the render_material_test.cpp / render_skinning_test.cpp precedent.
//
// THE RENDER INCLUDE IS THE UMBRELLA, DELIBERATELY, and it is not the house preference by accident:
// TM28 asserts that <aero/render/render.hpp> alone makes this task's four public names available, and
// a TU that also included the narrow headers could not witness that at all. The post_process.hpp arm
// of that claim is a genuine COMPILE failure if the umbrella line is deleted (seed T30); the
// tonemap.hpp arm is NOT, because tonemap_pack.hpp includes it directly -- that half is covered by
// the one-line grep recorded in the plan's V.4, and it is recorded as a PARTIAL pin rather than
// claimed as a full one.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the four-time trap in .claude/rules/ci-portability.md), and TM2 compares
// tonemapOperatorLabel's results.

#include <aero/assets/texture_cook.hpp>  // TM12: engine::assets::detail::{srgbToLinear,linearToSrgb}
#include <aero/core/math.hpp>
#include <aero/render/render.hpp>

#include "../engine/render/src/tonemap_pack.hpp"

#include <doctest/doctest.h>

#include <algorithm>  // std::clamp -- MSVC's STL supplies none of <algorithm> transitively
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>

using engine::Vec2;
using engine::Vec3;
using engine::render::ACES_A;
using engine::render::ACES_B;
using engine::render::ACES_C;
using engine::render::ACES_D;
using engine::render::ACES_E;
using engine::render::applyTonemapCurve;
using engine::render::linearToSrgbEncode;
using engine::render::MAX_EXPOSURE;
using engine::render::MIN_EXPOSURE;
using engine::render::sanitizeTonemapParams;
using engine::render::SRGB_ENCODE_GAMMA;
using engine::render::SRGB_ENCODE_OFFSET;
using engine::render::SRGB_ENCODE_SCALE;
using engine::render::SRGB_ENCODE_SLOPE;
using engine::render::SRGB_ENCODE_THRESHOLD;
using engine::render::TONEMAP_MAX_INPUT;
using engine::render::TONEMAP_OPERATOR_COUNT;
using engine::render::tonemapAndEncode;
using engine::render::TonemapOperator;
using engine::render::tonemapOperatorLabel;
using engine::render::TonemapParams;
using engine::render::tonemapSourceUvMax;

namespace {

constexpr float QNAN = std::numeric_limits<float>::quiet_NaN();
constexpr float POS_INF = std::numeric_limits<float>::infinity();
constexpr float NEG_INF = -std::numeric_limits<float>::infinity();

// A grey triple, so a per-channel defect that only hits x or z is still visible in the first channel.
[[nodiscard]] Vec3 grey(float v) noexcept { return Vec3{v, v, v}; }

[[nodiscard]] float curve1(float v, TonemapOperator op) noexcept { return applyTonemapCurve(grey(v), op).x; }

[[nodiscard]] float chain1(float v, float exposure, TonemapOperator op) noexcept {
    return tonemapAndEncode(grey(v), TonemapParams{exposure, op}).x;
}

// ------------------------------------------------------------------------------------------------
// TM29's machinery. A source line with its `//` comment removed -- render_skinning_test.cpp's JP14
// helper, copied rather than re-derived. The strip is LOAD-BEARING: tonemap.frag.hlsl's own header
// comment names engine/render/tonemap.hpp and its cbuffer comment quotes the three curve indices, so
// an un-stripped search would pass on prose.
// ------------------------------------------------------------------------------------------------
[[nodiscard]] std::string_view codeOf(std::string_view line) {
    const std::size_t commentStart = line.find("//");
    return commentStart == std::string_view::npos ? line : line.substr(0, commentStart);
}

[[nodiscard]] std::string strippedShaderSource(const std::string& relativePath) {
    const std::string path = std::string(AERO_SHADERS_SRC_DIR) + "/" + relativePath;
    std::ifstream stream{path, std::ios::binary};
    REQUIRE_MESSAGE(stream.good(), path);
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(text.empty());
    std::string out;
    out.reserve(text.size());
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        out.append(codeOf(line));
        out.push_back('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }
    return out;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] std::size_t countOf(const std::string& haystack, std::string_view needle) {
    std::size_t total = 0;
    std::size_t at = haystack.find(needle);
    while (at != std::string::npos) {
        ++total;
        at = haystack.find(needle, at + 1U);
    }
    return total;
}

// `after` occurs somewhere past `before`, and `before` occurs at all. Disengaged when either is
// absent, which the caller CHECKs against, so a renamed token fails loudly rather than vacuously.
[[nodiscard]] bool occursAfter(const std::string& haystack, std::string_view before, std::string_view after) {
    const std::size_t at = haystack.find(before);
    if (at == std::string::npos) {
        return false;
    }
    return haystack.find(after, at) != std::string::npos;
}

// The LAST line of the stripped source containing `return` -- the fragment stage's own final
// statement, which is where the literal alpha lives.
[[nodiscard]] std::string lastReturnLine(const std::string& code) {
    std::string found;
    std::string_view remaining = code;
    while (!remaining.empty()) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        if (line.find("return") != std::string_view::npos) {
            found = std::string{line};
        }
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }
    return found;
}

}  // namespace

// ================================================================================================
// Tier 0 -- the transfer chain. No GPU, no shader toolchain: these run in EVERY configuration.
// ================================================================================================

TEST_CASE("render tonemap: the operator's underlying values are the wire contract (TM1)") {
    // tonemap.frag.hlsl compares the RAW integer uCurve against literal 1 and 2, so these four
    // numbers are a wire format rather than an implementation detail (INV-5). tonemap_pack.hpp
    // carries the same four as static_asserts; this is the runtime mirror.
    CHECK(static_cast<std::uint8_t>(TonemapOperator::None) == 0);
    CHECK(static_cast<std::uint8_t>(TonemapOperator::Reinhard) == 1);
    CHECK(static_cast<std::uint8_t>(TonemapOperator::AcesApprox) == 2);
    CHECK(static_cast<std::uint8_t>(TonemapOperator::Count) == 3);
    CHECK(TONEMAP_OPERATOR_COUNT == 3);
}

TEST_CASE("render tonemap: the operator label is total, distinct and never empty (TM2)") {
    const std::string_view none = tonemapOperatorLabel(TonemapOperator::None);
    const std::string_view reinhard = tonemapOperatorLabel(TonemapOperator::Reinhard);
    const std::string_view aces = tonemapOperatorLabel(TonemapOperator::AcesApprox);

    CHECK(none == "None");
    CHECK(reinhard == "Reinhard");
    CHECK(aces == "ACES");

    CHECK_FALSE(none.empty());
    CHECK_FALSE(reinhard.empty());
    CHECK_FALSE(aces.empty());
    CHECK(none != reinhard);
    CHECK(none != aces);
    CHECK(reinhard != aces);

    // Count and any out-of-range cast get a distinct answer rather than one of the three real ones.
    const std::string_view sentinel = tonemapOperatorLabel(TonemapOperator::Count);
    CHECK(sentinel == "Unknown");
    CHECK(tonemapOperatorLabel(static_cast<TonemapOperator>(200)) == "Unknown");
}

TEST_CASE("render tonemap: the default params are {1.0, ACES} and compare field by field (TM3)") {
    const TonemapParams defaults{};
    CHECK(defaults.exposure == 1.0F);
    CHECK(defaults.curve == TonemapOperator::AcesApprox);
    // Already sanitized by construction -- editor_app.cpp's null-viewport arm hands this straight to
    // the preview.
    CHECK(sanitizeTonemapParams(defaults) == defaults);

    // TWO arms. One comparison that changes both fields witnesses neither.
    CHECK_FALSE(defaults == TonemapParams{2.0F, TonemapOperator::AcesApprox});
    CHECK_FALSE(defaults == TonemapParams{1.0F, TonemapOperator::Reinhard});
    CHECK(defaults == TonemapParams{1.0F, TonemapOperator::AcesApprox});
}

TEST_CASE("render tonemap: sanitize clamps exposure to both bounds and passes an in-range one through (TM4)") {
    CHECK(sanitizeTonemapParams({0.0F, TonemapOperator::None}).exposure == MIN_EXPOSURE);
    CHECK(sanitizeTonemapParams({-5.0F, TonemapOperator::None}).exposure == MIN_EXPOSURE);
    CHECK(sanitizeTonemapParams({1e9F, TonemapOperator::None}).exposure == MAX_EXPOSURE);
    // Bit-identical, not merely close: both bounds are powers of two and so is this value.
    CHECK(sanitizeTonemapParams({0.25F, TonemapOperator::Reinhard}).exposure == 0.25F);
    CHECK(sanitizeTonemapParams({MIN_EXPOSURE, TonemapOperator::None}).exposure == MIN_EXPOSURE);
    CHECK(sanitizeTonemapParams({MAX_EXPOSURE, TonemapOperator::None}).exposure == MAX_EXPOSURE);
}

TEST_CASE("render tonemap: a NaN exposure sanitizes to exactly 1.0, not to a bound (TM5)") {
    // NOT a clamp bound: NaN carries no direction, so neither MIN nor MAX is the honest answer. A
    // bare std::clamp here is a standard-library detail that differs between the three lanes, which
    // is exactly why the explicit NaN arm exists.
    const TonemapParams sane = sanitizeTonemapParams({QNAN, TonemapOperator::Reinhard});
    CHECK(sane.exposure == 1.0F);
    CHECK_FALSE(std::isnan(sane.exposure));
    CHECK(sane.curve == TonemapOperator::Reinhard);
}

TEST_CASE("render tonemap: +inf sanitizes to MAX_EXPOSURE and -inf to MIN_EXPOSURE (TM6)") {
    CHECK(sanitizeTonemapParams({POS_INF, TonemapOperator::None}).exposure == MAX_EXPOSURE);
    CHECK(sanitizeTonemapParams({NEG_INF, TonemapOperator::None}).exposure == MIN_EXPOSURE);
}

TEST_CASE("render tonemap: an out-of-range curve sanitizes to AcesApprox (TM7)") {
    for (const std::uint8_t raw : {std::uint8_t{3}, std::uint8_t{4}, std::uint8_t{255}}) {
        CAPTURE(raw);
        CHECK(sanitizeTonemapParams({1.0F, static_cast<TonemapOperator>(raw)}).curve == TonemapOperator::AcesApprox);
    }
    CHECK(sanitizeTonemapParams({1.0F, TonemapOperator::None}).curve == TonemapOperator::None);
    CHECK(sanitizeTonemapParams({1.0F, TonemapOperator::Reinhard}).curve == TonemapOperator::Reinhard);
    CHECK(sanitizeTonemapParams({1.0F, TonemapOperator::AcesApprox}).curve == TonemapOperator::AcesApprox);
}

TEST_CASE("render tonemap: the OETF's two arms meet at the ENCODE threshold, never the decode one (TM8)") {
    // The two constants are confusable precisely because 12.92 * 0.0031308 ~= 0.04045, which is the
    // DECODE threshold. Swapping them is the single most common defect in this function.
    CHECK(SRGB_ENCODE_THRESHOLD == 0.0031308F);
    CHECK(SRGB_ENCODE_THRESHOLD != 0.04045F);
    CHECK(SRGB_ENCODE_SLOPE == 12.92F);
    CHECK(SRGB_ENCODE_SCALE == 1.055F);
    CHECK(SRGB_ENCODE_OFFSET == 0.055F);
    CHECK(SRGB_ENCODE_GAMMA == 1.0F / 2.4F);

    const float linearArm = SRGB_ENCODE_SLOPE * SRGB_ENCODE_THRESHOLD;
    const float powerArm =
        (SRGB_ENCODE_SCALE * std::pow(SRGB_ENCODE_THRESHOLD, SRGB_ENCODE_GAMMA)) - SRGB_ENCODE_OFFSET;
    // ~6e-8 apart with the encode threshold; ~8e-3 apart with the decode one.
    CHECK(std::abs(linearArm - powerArm) <= 1.0e-6F);
    CHECK(std::abs(linearToSrgbEncode(SRGB_ENCODE_THRESHOLD) - linearArm) <= 1.0e-6F);
}

TEST_CASE("render tonemap: the encode is exact at 0 and 1, and the chain is finite for every input (TM9)") {
    CHECK(linearToSrgbEncode(0.0F) == 0.0F);  // 12.92 * 0 -- exact, and the only exact endpoint
    // f(1) is NOT exactly 1.0 in fp32, and that is arithmetic rather than a defect: 1.055F - 0.055F
    // rounds to 0.99999994, one ULP below one. Measured, not assumed. What is asserted instead is the
    // DIRECTION and the magnitude of the error -- an OETF that overshot would hand the 8-bit
    // conversion a value outside the [0,1] domain the whole chain guarantees.
    CHECK(linearToSrgbEncode(1.0F) <= 1.0F);
    CHECK(std::abs(linearToSrgbEncode(1.0F) - 1.0F) <= 1.0e-6F);

    for (const auto op : {TonemapOperator::None, TonemapOperator::Reinhard, TonemapOperator::AcesApprox}) {
        CAPTURE(static_cast<int>(op));
        for (const float v : {POS_INF, NEG_INF, TONEMAP_MAX_INPUT, 1.0e30F}) {
            CAPTURE(v);
            const Vec3 out = tonemapAndEncode(grey(v), TonemapParams{1.0F, op});
            CHECK(std::isfinite(out.x));
            CHECK(std::isfinite(out.y));
            CHECK(std::isfinite(out.z));
        }
    }
}

TEST_CASE("render tonemap: 0.21404 linear encodes to sRGB 0.5, the mid-grey anchor (TM10)") {
    // The value that the sRGB OETF maps to exactly one half. This is the number the sample's headline
    // row and validation row 6 are built on: --raw reads it at 55/255, the pass reads it at ~127/255.
    CHECK(std::abs(linearToSrgbEncode(0.21404F) - 0.5F) <= 2.0e-3F);
    // glTF's middle grey, the second anchor.
    CHECK(std::abs(linearToSrgbEncode(0.18F) - 0.4613561F) <= 1.0e-5F);
}

TEST_CASE("render tonemap: the OETF is monotonic non-decreasing across [0,1] (TM11)") {
    constexpr int SAMPLES = 4096;
    float previous = linearToSrgbEncode(0.0F);
    for (int i = 1; i <= SAMPLES; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(SAMPLES);
        const float y = linearToSrgbEncode(x);
        REQUIRE(y >= previous);
        previous = y;
    }
    CHECK(previous == doctest::Approx(1.0F));
}

TEST_CASE("render tonemap: the OETF agrees with the committed sRGB table at all 256 points (TM12)") {
    // Ties this OETF to the one 3.3.2 verified against the spec and that `ktx validate` exercises
    // continuously in the cook-determinism CI job. engine::assets::detail:: is one namespace deeper
    // than the shorthand, and texture_cook.hpp sanctions exactly one consumer -- aero_tests, which is
    // this binary.
    for (int v = 0; v < 256; ++v) {
        CAPTURE(v);
        const std::uint16_t linear16 = engine::assets::detail::srgbToLinear(static_cast<std::uint8_t>(v));
        const float linear = static_cast<float>(linear16) / 65535.0F;
        const float encoded = linearToSrgbEncode(linear);
        CHECK(std::abs((encoded * 255.0F) - static_cast<float>(v)) <= 1.0F);
    }
    // ...and the table's own inverse still round-trips, so a broken table would not be mistaken for a
    // broken OETF.
    CHECK(engine::assets::detail::linearToSrgb(engine::assets::detail::srgbToLinear(128)) == 128);
}

TEST_CASE("render tonemap: the None curve is the identity on [0,1] and saturates above it (TM13)") {
    for (const float v : {0.0F, 0.25F, 0.5F, 1.0F}) {
        CAPTURE(v);
        CHECK(curve1(v, TonemapOperator::None) == v);  // bit-identical
    }
    CHECK(curve1(2.0F, TonemapOperator::None) == 1.0F);
    CHECK(curve1(-1.0F, TonemapOperator::None) == 0.0F);
}

TEST_CASE("render tonemap: Reinhard is x/(1+x) -- f(1) is exactly one half (TM14)") {
    CHECK(curve1(0.0F, TonemapOperator::Reinhard) == 0.0F);
    CHECK(curve1(1.0F, TonemapOperator::Reinhard) == 0.5F);  // exactly, not approximately

    float previous = curve1(0.0F, TonemapOperator::Reinhard);
    for (int i = 1; i <= 512; ++i) {
        const float x = static_cast<float>(i) * 0.05F;
        const float y = curve1(x, TonemapOperator::Reinhard);
        REQUIRE(y >= previous);
        previous = y;
    }
    // Asymptotic to 1 from below in REAL arithmetic. In fp32 the asymptote IS reachable, because
    // 1.0F + x == x once x >= 2^24, so f(1e30F) is exactly 1.0 -- measured, and recorded here rather
    // than asserted away. What the curve must guarantee is that it never EXCEEDS one, and that at the
    // HDR buffer's own ceiling it is still strictly below it (65504/65505 = 0.99998474).
    CHECK(curve1(1.0e30F, TonemapOperator::Reinhard) <= 1.0F);
    CHECK(curve1(TONEMAP_MAX_INPUT, TonemapOperator::Reinhard) < 1.0F);
}

TEST_CASE("render tonemap: the ACES fit starts at zero, rises monotonically and saturates (TM15)") {
    CHECK(curve1(0.0F, TonemapOperator::AcesApprox) == 0.0F);

    float previous = curve1(0.0F, TonemapOperator::AcesApprox);
    for (int i = 1; i <= 512; ++i) {
        const float x = static_cast<float>(i) * 0.02F;
        const float y = curve1(x, TonemapOperator::AcesApprox);
        REQUIRE(y >= previous);
        previous = y;
    }
    CHECK(curve1(1.0e6F, TonemapOperator::AcesApprox) == 1.0F);
}

TEST_CASE("render tonemap: ACES f(1) is 2.54/3.16, pinned as a literal (TM16)") {
    // (2.51 + 0.03) / ((2.43 + 0.59) + 0.14) -- a LITERAL, not the same expression the code computes.
    CHECK(std::abs(curve1(1.0F, TonemapOperator::AcesApprox) - 0.80379747F) <= 1.0e-5F);
}

TEST_CASE("render tonemap: ACES f(0.18) is the middle-grey anchor, pinned as a literal (TM17)") {
    CHECK(std::abs(curve1(0.18F, TonemapOperator::AcesApprox) - 0.2668989F) <= 1.0e-5F);
}

TEST_CASE("render tonemap: the five Narkowicz constants are 2.51 / 0.03 / 2.43 / 0.59 / 0.14 (TM18)") {
    CHECK(ACES_A == 2.51F);
    CHECK(ACES_B == 0.03F);
    CHECK(ACES_C == 2.43F);
    CHECK(ACES_D == 0.59F);
    CHECK(ACES_E == 0.14F);
}

TEST_CASE("render tonemap: the chain's ORDER is the contract -- three independent arms (TM19)") {
    SUBCASE("(a) exposure is applied BEFORE the curve") {
        // Both reach the curve as exactly 1.0, so on CORRECT code the two are EQUAL. The polarity is
        // inverted from the obvious reading and that is the point: applying exposure AFTER the curve
        // gives Reinhard(0.5)*2 = 0.6667 against Reinhard(1.0) = 0.5, which are NOT equal.
        const float before = chain1(0.5F, 2.0F, TonemapOperator::Reinhard);
        const float after = chain1(1.0F, 1.0F, TonemapOperator::Reinhard);
        CHECK(before == after);
        // ...and the pair genuinely discriminates: a different exposure gives a different answer, so
        // the equality above is not the trivial one an all-constant function would satisfy.
        CHECK(chain1(0.5F, 1.0F, TonemapOperator::Reinhard) != after);
    }
    SUBCASE("(b) the max(0) step is present") {
        // Written against the ACES arm at -0.5: Reinhard's own pole saturates to 0 either way, so it
        // cannot see the guard at all. ACES at -0.5 gives 0.6125/0.4525 = 1.354 -> saturates to 1.0
        // WITHOUT the guard, and 0.0 with it.
        CHECK(chain1(-0.5F, 1.0F, TonemapOperator::AcesApprox) == 0.0F);
        CHECK(chain1(-1.0F, 1.0F, TonemapOperator::Reinhard) == 0.0F);
    }
    SUBCASE("(c) the min(TONEMAP_MAX_INPUT) step is present") {
        // BIT-IDENTICAL: without the clamp, 1e30 squared overflows fp32 to inf in both the ACES
        // numerator and denominator, and inf/inf is NaN -- which is never equal to anything.
        const Vec3 huge = tonemapAndEncode(grey(1.0e30F), TonemapParams{1.0F, TonemapOperator::AcesApprox});
        const Vec3 capped = tonemapAndEncode(grey(TONEMAP_MAX_INPUT), TonemapParams{1.0F, TonemapOperator::AcesApprox});
        CHECK(huge.x == capped.x);
        CHECK(huge.y == capped.y);
        CHECK(huge.z == capped.z);
    }
}

TEST_CASE("render tonemap: the ACES fit's origin slope is ACES_B/ACES_E, NOT one (TM20)") {
    // THE EPSILON IS PART OF THE ASSERTION. Measured at 1e-6 the fit reads 0.2142999 (0.007% off the
    // analytic 0.03/0.14 = 0.2142857); at 1e-4 it already reads 0.215990, which is 0.8% off and would
    // make a 1% tolerance meaningless. The fit is NOT near-identity in the shadows -- a linear 0.02
    // maps to ~0.0105 -- and that is the fit's own character, not a defect. DO NOT "FIX" IT: if a
    // validation pass judges the default too dark, the fix is the default EXPOSURE.
    constexpr float EPS = 1.0e-6F;
    const float slope = curve1(EPS, TonemapOperator::AcesApprox) / EPS;
    const float analytic = ACES_B / ACES_E;
    CHECK(std::abs(slope - analytic) <= 0.01F * analytic);
    CHECK(slope < 0.5F);  // emphatically not an identity slope
}

// ================================================================================================
// TM21-TM24 -- the source sub-rect rule, and TM25-TM27 -- the two uniform packers.
// ================================================================================================

TEST_CASE("render tonemap: equal draw and texture extents give a full [0,1] sub-rect (TM21)") {
    const Vec2 uv = tonemapSourceUvMax({256, 192}, {256, 192});
    CHECK(uv.x == 1.0F);
    CHECK(uv.y == 1.0F);
}

TEST_CASE("render tonemap: an over-allocated target's sub-rect is draw/texture per axis (TM22)") {
    // 200/256 == 150/192 == 0.78125, exactly representable in binary -- so this is an equality, not
    // an approximation, and it is a LITERAL rather than the same division the code performs.
    const Vec2 uv = tonemapSourceUvMax({200, 150}, {256, 192});
    CHECK(uv.x == 0.78125F);
    CHECK(uv.y == 0.78125F);
}

TEST_CASE("render tonemap: a zero texture extent gives 1.0, never a division by zero (TM23)") {
    // A not-renderable target. Nothing is drawn from it anyway, so 1.0 is the only answer that is
    // not a division by zero.
    const Vec2 both = tonemapSourceUvMax({200, 150}, {0, 0});
    CHECK(both.x == 1.0F);
    CHECK(both.y == 1.0F);
    // Per axis, independently -- a shared early return would make these two indistinguishable.
    const Vec2 widthOnly = tonemapSourceUvMax({200, 150}, {0, 192});
    CHECK(widthOnly.x == 1.0F);
    CHECK(widthOnly.y == 0.78125F);
    const Vec2 heightOnly = tonemapSourceUvMax({200, 150}, {256, 0});
    CHECK(heightOnly.x == 0.78125F);
    CHECK(heightOnly.y == 1.0F);
}

TEST_CASE("render tonemap: a draw extent larger than its allocation clamps per axis (TM24)") {
    // INV-1 violated. It cannot happen under the adjacent-resize rule, and if it ever does, sampling
    // past 1.0 would read the unrendered margin -- so the min() handles it rather than asserting it.
    const Vec2 both = tonemapSourceUvMax({300, 300}, {256, 192});
    CHECK(both.x == 1.0F);
    CHECK(both.y == 1.0F);
    // The two axes clamp INDEPENDENTLY: one over, one under.
    const Vec2 mixed = tonemapSourceUvMax({300, 150}, {256, 192});
    CHECK(mixed.x == 1.0F);
    CHECK(mixed.y == 0.78125F);
}

TEST_CASE("render tonemap: the vertex block is {uvScale.x, uvScale.y, 0, 0} (TM25)") {
    const auto block = engine::render::detail::packTonemapVertex(engine::Vec2{0.75F, 0.5F});
    CHECK(block.size() == engine::render::detail::TONEMAP_VERTEX_UNIFORM_BYTES);
    CHECK(block.size() == 16);

    float first = 0.0F;
    float second = 0.0F;
    std::memcpy(&first, block.data() + 0, sizeof(float));
    std::memcpy(&second, block.data() + 4, sizeof(float));
    CHECK(first == 0.75F);
    CHECK(second == 0.5F);

    // Every padding byte ZERO, not merely "whatever the compiler left". SDL copies the block verbatim
    // into its uniform ring, and an indeterminate tail byte is a value that differs between runs on
    // the same machine.
    for (std::size_t i = 8; i < block.size(); ++i) {
        CAPTURE(i);
        CHECK(block[i] == std::byte{0});
    }
}

TEST_CASE("render tonemap: the fragment block is {exposure, raw curve, 0, 0} (TM26)") {
    const auto block = engine::render::detail::packTonemapFragment({2.0F, TonemapOperator::Reinhard});
    CHECK(block.size() == engine::render::detail::TONEMAP_FRAGMENT_UNIFORM_BYTES);
    CHECK(block.size() == 16);

    float exposure = 0.0F;
    std::uint32_t curve = 0;
    std::memcpy(&exposure, block.data() + 0, sizeof(float));
    std::memcpy(&curve, block.data() + 4, sizeof(std::uint32_t));
    CHECK(exposure == 2.0F);
    // Decoded as a uint32, against the LITERAL the HLSL compares against -- not against a cast of the
    // same enum the packer wrote.
    CHECK(curve == 1);

    for (std::size_t i = 8; i < block.size(); ++i) {
        CAPTURE(i);
        CHECK(block[i] == std::byte{0});
    }

    // The whole enum round-trips as the raw integer the fragment stage's chained ternary reads.
    const auto packCurve = [](TonemapOperator op) {
        const auto bytes = engine::render::detail::packTonemapFragment({1.0F, op});
        std::uint32_t raw = 0;
        std::memcpy(&raw, bytes.data() + 4, sizeof(std::uint32_t));
        return raw;
    };
    CHECK(packCurve(TonemapOperator::None) == 0);
    CHECK(packCurve(TonemapOperator::Reinhard) == 1);
    CHECK(packCurve(TonemapOperator::AcesApprox) == 2);
}

TEST_CASE("render tonemap: the packer does NOT sanitize -- the caller's contract, pinned (TM27)") {
    // PostProcess::resolve is the one production caller and it sanitizes BEFORE either pack (INV-3).
    // This case exists so that contract is explicit rather than assumed: a packer that silently
    // clamped would hide an unsanitized call site instead of letting resolve's own step be the thing
    // that guarantees it.
    const auto block = engine::render::detail::packTonemapFragment({1000.0F, TonemapOperator::None});
    float exposure = 0.0F;
    std::memcpy(&exposure, block.data() + 0, sizeof(float));
    CHECK(exposure == 1000.0F);  // the RAW value, not MAX_EXPOSURE
    CHECK(exposure != MAX_EXPOSURE);
    CHECK(std::isfinite(exposure));

    // ...and resolve's own step is what turns that into a safe uniform.
    const auto sane =
        engine::render::detail::packTonemapFragment(sanitizeTonemapParams({1000.0F, TonemapOperator::None}));
    float clamped = 0.0F;
    std::memcpy(&clamped, sane.data() + 0, sizeof(float));
    CHECK(clamped == MAX_EXPOSURE);
}

TEST_CASE("render tonemap: the umbrella header alone names this task's public surface (TM28)") {
    // THIS TU INCLUDES <aero/render/render.hpp> AND NEITHER NARROW HEADER (see the file-top note), so
    // the post_process.hpp arm of this claim is a genuine COMPILE failure if that umbrella line is
    // deleted. The tonemap.hpp arm is NOT -- tonemap_pack.hpp includes it directly -- and is covered
    // by a one-line grep over render.hpp instead. Recorded as a PARTIAL pin rather than claimed as a
    // full one.
    static_assert(std::is_default_constructible_v<engine::render::TonemapParams>);
    static_assert(std::is_default_constructible_v<engine::render::PostProcessConfig>);
    static_assert(std::is_enum_v<engine::render::TonemapOperator>);
    // DOUBLE PARENTHESES on both format comparisons, and they are not stylistic: engine::rhi carries
    // a toString(TextureFormat) on a public header, doctest's DOCTEST_STRINGIFY expands to an
    // UNQUALIFIED toString(...) that ADL finds there, and the decomposer then tries
    // std::string_view + const char* -- a hard compile error on EVERY lane, reported inside doctest.h.
    // The extra parentheses stop the decomposition. This is the render_target_test.cpp idiom, and it
    // is exactly why this task's own label function is named tonemapOperatorLabel.
    CHECK((engine::render::PostProcessConfig{}.sceneColorFormat == engine::rhi::TextureFormat::RGBA16Float));
    CHECK((engine::render::PostProcessConfig{}.outputColorFormat == engine::rhi::TextureFormat::Invalid));
    CHECK(engine::render::PostProcessConfig{}.sceneDepth);
    CHECK(engine::render::tonemapSourceUvMax({1, 1}, {1, 1}).x == 1.0F);
}

// ================================================================================================
// TM29 -- the shader source-text pin. There is no behaviour to observe: the HLSL and the C++ never
// see each other's tokens, and EVERY mismatched variant compiles, cooks, submits and draws. Reached
// through AERO_SHADERS_SRC_DIR (the SOURCE tree, distinct from AERO_SHADERS_DIR's build output), and
// UNGATED -- the HLSL text exists whether or not AERO_SHADER_TOOLS built it.
// ================================================================================================

TEST_CASE("render tonemap: the HLSL transcribes tonemap.hpp, pinned as source text (TM29)") {
    const std::string frag = strippedShaderSource("tonemap.frag.hlsl");
    const std::string vert = strippedShaderSource("fullscreen.vert.hlsl");

    // (a) NON-VACUITY, first direction: the scan found something at all. Without this a path typo
    //     would make every "contains" below fail with a message naming the needle instead of the path.
    REQUIRE_FALSE(frag.empty());
    REQUIRE_FALSE(vert.empty());

    // (b) NON-VACUITY, second direction: a deliberately-absent needle IS absent, proving the search
    //     can say "no". Without this, a `contains` that always returned true would satisfy every
    //     clause below. 0.04045 is the sRGB DECODE threshold -- seed T2b's landing site.
    CHECK_FALSE(contains(frag, "0.04045"));
    CHECK_FALSE(contains(frag, "tonemapFilmic"));

    SUBCASE("the five Narkowicz constants") {
        CHECK(contains(frag, "2.51"));
        CHECK(contains(frag, "0.03"));
        CHECK(contains(frag, "2.43"));
        CHECK(contains(frag, "0.59"));
        CHECK(contains(frag, "0.14"));
    }
    SUBCASE("the five sRGB OETF constants") {
        CHECK(contains(frag, "0.0031308"));
        CHECK(contains(frag, "12.92"));
        CHECK(contains(frag, "1.055"));
        CHECK(contains(frag, "0.055"));
        CHECK(contains(frag, "1.0 / 2.4"));
    }
    SUBCASE("TONEMAP_MAX_INPUT, the largest finite half") { CHECK(contains(frag, "65504")); }
    SUBCASE("the OETF is both DEFINED and CALLED") {
        // At least twice: once as the definition, once in the final return. A pin that only found the
        // definition would stay green under a seed that deleted the call (T1).
        CHECK(countOf(frag, "linearToSrgbEncode") >= 2);
    }
    SUBCASE("the curve arms are in the order the enum's values demand") {
        CHECK(occursAfter(frag, "uCurve == 1", "tonemapReinhard"));
        CHECK(occursAfter(frag, "uCurve == 2", "tonemapAcesApprox"));
    }
    SUBCASE("the fragment stage writes a LITERAL alpha, never the sampled one") {
        // ImGui::Image alpha-blends this texture over the panel background, so a sampled alpha of 0
        // would make the whole viewport transparent (INV-6).
        const std::string lastReturn = lastReturnLine(frag);
        REQUIRE_FALSE(lastReturn.empty());
        CHECK(lastReturn.find(", 1.0)") != std::string::npos);
        CHECK(lastReturn.find(".a") == std::string::npos);
    }
    SUBCASE("the fullscreen vertex stage's NDC formula and its uv scale") {
        CHECK(contains(vert, "SV_VertexID"));
        // Twice: the cbuffer declaration and the multiply. One occurrence means the uniform is
        // declared and never used, which is a full-texture sample of an over-allocated target.
        CHECK(countOf(vert, "uUvScale") >= 2);
        // The vertical flip. Getting the Y sign wrong is the classic upside-down picture and NO
        // AUTOMATED TIER IN THIS TREE CAN SEE IT -- these two literals are the whole mechanical cover.
        CHECK(contains(vert, "float2(2.0, -2.0)"));
        CHECK(contains(vert, "float2(-1.0, 1.0)"));
    }
}

// ================================================================================================
// Tier 1 -- a real Device, no window. PostProcess's cycle, both latches, the refusal asymmetry.
//
// Gated on AERO_SHADER_TOOLS_ENABLED for the reason render_culling_test.cpp's and
// render_skinning_test.cpp's own tier-1 blocks are: a PostProcess loads its two shaders from
// build/<preset>/shaders, which only exists when the shader toolchain is built. The whole TM battery
// above runs in every configuration.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/rhi/rhi.hpp>

    #include "rhi_test_support.hpp"

    #include <memory>
    #include <utility>

namespace {

// A DirectoryBackend that admits exactly one shader's artifacts. PP4 needs the arm where ONE of the
// two loads succeeds and the other does not, because an EMPTY VFS leaves both handles invalid and
// destroying an invalid handle is a documented no-op -- so an empty mount cannot see a create() that
// forgot to destroy the shader it DID make. This is the difference between a real leak witness and a
// decorative one (seed T25).
class SingleShaderBackend final : public engine::FileSystemBackend {
public:
    SingleShaderBackend(std::string_view rootDirectory, std::string_view allowedPrefix)
        : inner(rootDirectory), prefix(allowedPrefix) {}

    [[nodiscard]] bool exists(std::string_view relPath) const override {
        return admits(relPath) && inner.exists(relPath);
    }
    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        return admits(relPath) ? inner.fileSize(relPath) : std::nullopt;
    }
    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        return admits(relPath) ? inner.readFile(relPath) : std::nullopt;
    }

private:
    [[nodiscard]] bool admits(std::string_view relPath) const { return relPath.starts_with(prefix); }

    engine::DirectoryBackend inner;
    std::string prefix;
};

// beginScene -> [draw] -> endScene (submits A) -> out.beginFrame -> resolve -> out.endFrame (B).
// A is submitted BEFORE B is acquired, so the queue-ordering guarantee applies with no interleaving
// at all -- which is why no explicit barrier is needed and none is available.
void cycleOnce(engine::render::PostProcess& post, engine::render::RenderTarget& out, const TonemapParams& params) {
    std::optional<engine::render::Frame> sceneFrame = post.beginScene({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(sceneFrame.has_value());
    CHECK(post.endScene(std::move(*sceneFrame)));
    std::optional<engine::render::Frame> outFrame = out.beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(outFrame.has_value());
    post.resolve(*outFrame, params);
    CHECK(out.endFrame(std::move(*outFrame)));
}

}  // namespace

    // The tier-1 preamble, written out per case exactly as render_culling_test.cpp does it:
    // AERO_SKIP_OR_FAIL returns from the enclosing function, so it cannot live in a helper.
    #define AERO_TONEMAP_TIER1_PREAMBLE()                                                                  \
        const engine::platform::Context ctx{{.headless = false}};                                          \
        if (!ctx.valid()) {                                                                                \
            AERO_SKIP_OR_FAIL("no real video driver available");                                           \
        }                                                                                                  \
        auto device = engine::rhi::Device::create();                                                       \
        if (!device.has_value()) {                                                                         \
            AERO_SKIP_OR_FAIL("no GPU device available");                                                  \
        }                                                                                                  \
        engine::VirtualFileSystem vfs;                                                                     \
        vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));                           \
        auto out = engine::render::RenderTarget::create(                                                   \
            *device, {256, 192}, {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = false}); \
        REQUIRE(out.has_value())

TEST_CASE("render tonemap: a created pass owns an RGBA16Float scene target with depth (PP1)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    auto post = engine::render::PostProcess::create(*device, vfs, {256, 192},
                                                    {.outputColorFormat = out->colorFormat(), .quantum = 1});
    REQUIRE(post.has_value());

    // THE HDR-NESS OF THE INTERMEDIATE HAS EXACTLY ONE AUTOMATED WITNESS, and this is it. Losing it
    // would silently restore the clipping this whole task exists to remove, with every other case
    // still green.
    CHECK((post->sceneColorFormat() == engine::rhi::TextureFormat::RGBA16Float));
    CHECK((post->sceneDepthFormat() != engine::rhi::TextureFormat::Invalid));
    CHECK((post->sceneDrawExtent() == engine::rhi::Extent2D{256, 192}));
    CHECK((post->sceneTextureExtent() == engine::rhi::Extent2D{256, 192}));  // quantum 1 => equal
    CHECK(post->resolveCount() == 0);
    CHECK_FALSE(post->hasWarnedExtentMismatch());
    CHECK_FALSE(post->hasWarnedResolveBeforeEndScene());
}

TEST_CASE("render tonemap: create refuses an Invalid or depth outputColorFormat (PP2)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    CHECK_FALSE(engine::render::PostProcess::create(*device, vfs, {256, 192},
                                                    {.outputColorFormat = engine::rhi::TextureFormat::Invalid})
                    .has_value());
    CHECK_FALSE(engine::render::PostProcess::create(*device, vfs, {256, 192},
                                                    {.outputColorFormat = engine::rhi::TextureFormat::D32Float})
                    .has_value());
}

TEST_CASE("render tonemap: create refuses a depth sceneColorFormat, through RenderTarget (PP3)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    // RenderTarget::create's OWN refusal, reached THROUGH this config rather than duplicated here --
    // which is the point: PostProcess adds no second copy of a policy that already exists.
    CHECK_FALSE(engine::render::PostProcess::create(
                    *device, vfs, {256, 192},
                    {.outputColorFormat = out->colorFormat(), .sceneColorFormat = engine::rhi::TextureFormat::D32Float})
                    .has_value());
}

TEST_CASE("render tonemap: create refuses a missing shader and leaks nothing (PP4)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    SUBCASE("neither shader resolves") {
        const engine::VirtualFileSystem emptyVfs;  // nothing mounted at all
        CHECK_FALSE(engine::render::PostProcess::create(*device, emptyVfs, {256, 192},
                                                        {.outputColorFormat = out->colorFormat()})
                        .has_value());
    }
    SUBCASE("ONLY the vertex shader resolves -- the arm that can see a leak") {
        // With both handles invalid the destroys are no-ops, so the empty-VFS arm above cannot
        // witness a create() that forgot to release the shader it DID create. Here `vs` is a real GPU
        // object and `fs` is not: skipping either destroy leaves a live shader at ~Device, which logs
        // a leak WARN and is what ASan sees on the Debug lanes.
        engine::VirtualFileSystem partialVfs;
        partialVfs.mount(std::make_unique<SingleShaderBackend>(AERO_SHADERS_DIR, "fullscreen.vert"));
        CHECK(partialVfs.exists("res://fullscreen.vert.json"));
        CHECK_FALSE(partialVfs.exists("res://tonemap.frag.json"));
        CHECK_FALSE(engine::render::PostProcess::create(*device, partialVfs, {256, 192},
                                                        {.outputColorFormat = out->colorFormat()})
                        .has_value());
    }
}

TEST_CASE("render tonemap: a ForwardRenderer builds against the pass's scene formats (PP5)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    auto post =
        engine::render::PostProcess::create(*device, vfs, {256, 192}, {.outputColorFormat = out->colorFormat()});
    REQUIRE(post.has_value());

    // The seam is REAL, not asserted. This is also the case that fails outright if sceneDepth were
    // ever defaulted false: ForwardRendererConfig rejects an Invalid depthFormat.
    auto forward = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = post->sceneColorFormat(), .depthFormat = post->sceneDepthFormat()});
    CHECK(forward.has_value());
}

TEST_CASE("render tonemap: one clean cycle resolves once and latches nothing (PP6)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    auto post =
        engine::render::PostProcess::create(*device, vfs, {256, 192}, {.outputColorFormat = out->colorFormat()});
    REQUIRE(post.has_value());

    cycleOnce(*post, *out, TonemapParams{});
    CHECK(post->resolveCount() == 1);
    CHECK_FALSE(post->hasWarnedExtentMismatch());
    CHECK_FALSE(post->hasWarnedResolveBeforeEndScene());
}

TEST_CASE("render tonemap: a mismatched extent still DRAWS and latches its warning (PP7)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    auto post =
        engine::render::PostProcess::create(*device, vfs, {256, 192}, {.outputColorFormat = out->colorFormat()});
    REQUIRE(post.has_value());

    // ONE SEQUENCE CASE, deliberately. The "still draws" half and the "latches" half both have to
    // hold in the SAME run: two independent cases each pass under a defect that breaks the other.
    CHECK(post->resize({128, 96}));
    CHECK((post->sceneDrawExtent() == engine::rhi::Extent2D{128, 96}));
    cycleOnce(*post, *out, TonemapParams{});  // out is still 256x192
    CHECK(post->resolveCount() == 1);
    CHECK(post->hasWarnedExtentMismatch());
}

TEST_CASE("render tonemap: a resolve before endScene still DRAWS and latches its warning (PP8)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    auto post =
        engine::render::PostProcess::create(*device, vfs, {256, 192}, {.outputColorFormat = out->colorFormat()});
    REQUIRE(post.has_value());

    // The violation is DRIVEN, not merely asserted absent: a latch that can never fire satisfies
    // "did not fire", which is exactly why PP6's negative assertions are not enough on their own.
    std::optional<engine::render::Frame> sceneFrame = post->beginScene({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(sceneFrame.has_value());
    std::optional<engine::render::Frame> outFrame = out->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(outFrame.has_value());
    post->resolve(*outFrame, TonemapParams{});
    CHECK(out->endFrame(std::move(*outFrame)));
    CHECK(post->endScene(std::move(*sceneFrame)));  // close the stray scene frame

    CHECK(post->resolveCount() == 1);  // it DREW -- refusing would have been the wrong answer
    CHECK(post->hasWarnedResolveBeforeEndScene());
    CHECK_FALSE(post->hasWarnedExtentMismatch());

    // A correct cycle afterwards still draws, and the latch stays latched rather than resetting.
    cycleOnce(*post, *out, TonemapParams{});
    CHECK(post->resolveCount() == 2);
    CHECK(post->hasWarnedResolveBeforeEndScene());
}

TEST_CASE("render tonemap: resize propagates the quantum and the max extent (PP9)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    auto post = engine::render::PostProcess::create(
        *device, vfs, {100, 100}, {.outputColorFormat = out->colorFormat(), .quantum = 64, .maxExtent = 512});
    REQUIRE(post.has_value());

    CHECK(post->resize({100, 100}));
    CHECK((post->sceneDrawExtent() == engine::rhi::Extent2D{100, 100}));
    CHECK((post->sceneTextureExtent() == engine::rhi::Extent2D{128, 128}));
    // ...and the sub-rect rule reads exactly that pair, which is the whole reason the pass owns the
    // target rather than handing a texture handle out.
    const Vec2 uv = tonemapSourceUvMax(post->sceneDrawExtent(), post->sceneTextureExtent());
    CHECK(uv.x == 100.0F / 128.0F);
    CHECK(uv.y == 100.0F / 128.0F);

    CHECK(post->resize({900, 900}));
    CHECK((post->sceneDrawExtent() == engine::rhi::Extent2D{512, 512}));
}

TEST_CASE("render tonemap: a moved-from pass refuses without moving resolveCount (PP10)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    auto post =
        engine::render::PostProcess::create(*device, vfs, {256, 192}, {.outputColorFormat = out->colorFormat()});
    REQUIRE(post.has_value());

    engine::render::PostProcess moved{std::move(*post)};
    cycleOnce(moved, *out, TonemapParams{});
    CHECK(moved.resolveCount() == 1);

    // THE ASYMMETRY: every other refusal still draws. A not-renderable pass is the one that does not,
    // and resolveCount() UNMOVED is the whole observable -- there is deliberately no third accessor.
    std::optional<engine::render::Frame> outFrame = out->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(outFrame.has_value());
    post->resolve(*outFrame, TonemapParams{});  // NOLINT(bugprone-use-after-move) -- deliberate
    CHECK(post->resolveCount() == 0);
    CHECK(out->endFrame(std::move(*outFrame)));

    // Move-assign OVER a live pass, then destroy: a defaulted move would double-free the pipeline and
    // the sampler here, and ASan on the Debug lanes is what sees it.
    auto second =
        engine::render::PostProcess::create(*device, vfs, {128, 128}, {.outputColorFormat = out->colorFormat()});
    REQUIRE(second.has_value());
    *second = std::move(moved);
    CHECK(second->resolveCount() == 1);
    CHECK((second->sceneDrawExtent() == engine::rhi::Extent2D{256, 192}));
}

TEST_CASE("render tonemap: two passes live on one device, each resolving its own output (PP11)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    // The editor's real shape: the viewport panel and the material preview each own a PostProcess and
    // each resolve into their own target, in the same frame, on the same device.
    auto secondOut = engine::render::RenderTarget::create(
        *device, {128, 128}, {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = false});
    REQUIRE(secondOut.has_value());

    auto first =
        engine::render::PostProcess::create(*device, vfs, {256, 192}, {.outputColorFormat = out->colorFormat()});
    REQUIRE(first.has_value());
    auto second =
        engine::render::PostProcess::create(*device, vfs, {128, 128}, {.outputColorFormat = secondOut->colorFormat()});
    REQUIRE(second.has_value());

    cycleOnce(*first, *out, TonemapParams{1.0F, TonemapOperator::AcesApprox});
    cycleOnce(*second, *secondOut, TonemapParams{2.0F, TonemapOperator::Reinhard});

    CHECK(first->resolveCount() == 1);
    CHECK(second->resolveCount() == 1);
    CHECK_FALSE(first->hasWarnedExtentMismatch());
    CHECK_FALSE(second->hasWarnedExtentMismatch());
    CHECK_FALSE(first->hasWarnedResolveBeforeEndScene());
    CHECK_FALSE(second->hasWarnedResolveBeforeEndScene());
}

TEST_CASE("render tonemap: both output-depth arms record cleanly (PP12)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    SUBCASE("(a) a depth-FREE output, the two editor consumers' shape") {
        auto post = engine::render::PostProcess::create(
            *device, vfs, {256, 192},
            {.outputColorFormat = out->colorFormat(), .outputDepthFormat = out->depthFormat()});
        REQUIRE(post.has_value());
        CHECK((out->depthFormat() == engine::rhi::TextureFormat::Invalid));
        cycleOnce(*post, *out, TonemapParams{});
        CHECK(post->resolveCount() == 1);
    }
    SUBCASE("(b) a depth-CARRYING output -- the arm the SAMPLE uses in production") {
        // RendererConfig{.depth = true} gives the swapchain frame a depth attachment, so this is not
        // a hypothetical branch: it is the one both editor consumers do NOT exercise and the sample
        // does, and it is the whole reason PostProcessConfig::outputDepthFormat exists.
        auto depthOut = engine::render::RenderTarget::create(
            *device, {256, 192}, {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = true});
        REQUIRE(depthOut.has_value());
        CHECK((depthOut->depthFormat() != engine::rhi::TextureFormat::Invalid));

        auto post = engine::render::PostProcess::create(
            *device, vfs, {256, 192},
            {.outputColorFormat = depthOut->colorFormat(), .outputDepthFormat = depthOut->depthFormat()});
        REQUIRE(post.has_value());
        cycleOnce(*post, *depthOut, TonemapParams{});
        CHECK(post->resolveCount() == 1);
        CHECK_FALSE(post->hasWarnedExtentMismatch());
    }
}

TEST_CASE("render tonemap: ten consecutive resizes with adjacent updates latch nothing (PP13)") {
    AERO_TONEMAP_TIER1_PREAMBLE();

    auto post =
        engine::render::PostProcess::create(*device, vfs, {200, 150}, {.outputColorFormat = out->colorFormat()});
    REQUIRE(post.has_value());

    // The editor's continuous-resize case, and the one that would catch an off-by-one in the
    // ADJACENCY rule -- both targets are resized from the SAME value on adjacent lines, which is what
    // makes the 1:1 blit true by construction rather than by two call sites that could drift.
    for (std::uint32_t i = 0; i < 10; ++i) {
        const engine::rhi::Extent2D pixels{200 + i, 150 + i};
        CHECK(post->resize(pixels));
        CHECK(out->resize(pixels));
        cycleOnce(*post, *out, TonemapParams{});
    }
    CHECK(post->resolveCount() == 10);
    CHECK_FALSE(post->hasWarnedExtentMismatch());
    CHECK_FALSE(post->hasWarnedResolveBeforeEndScene());
}

#endif  // AERO_SHADER_TOOLS_ENABLED
