// engine/render/src/tonemap.cpp — task 3.6.3: the transfer chain behind tonemap.hpp. Pure arithmetic
// over engine math types. Nothing here allocates, logs, recurses, touches a GPU or holds static
// mutable state, and there is no profiling include either (the culling.cpp / animation.cpp absence:
// this is a handful of float operations and a Tracy zone would cost more than the work it measured).
// TOTALITY, STATED EXACTLY, because "total" and "finite" are not the same claim and an earlier
// version of this line conflated them. sanitizeTonemapParams and tonemapOperatorLabel are total in
// the strong sense: NaN, +-inf and an out-of-range enum each yield a defined, in-range answer.
// linearToSrgbEncode, applyTonemapCurve and tonemapAndEncode are total in the weak sense -- no UB, no
// trap, a defined answer for every input -- and FINITE for every FINITE input and for +-inf, but they
// PROPAGATE a NaN input rather than mapping it. That is deliberate and it is not a gap in the guards:
// min/max/clamp are all specified in terms of `<`, which is false in both directions for NaN, so each
// returns its NaN operand on every standard library. Mapping NaN here would make this function a
// DIFFERENT function from shaders/tonemap.frag.hlsl, which cannot follow -- HLSL's min/max with NaN
// are implementation-defined -- and TM29 exists precisely to keep the two the same. TM9 pins both
// halves; tonemap.hpp's property 3 states the contract.

#include <aero/render/tonemap.hpp>

#include <algorithm>  // std::clamp / std::min / std::max -- MSVC's STL supplies none of it transitively
#include <cmath>
#include <cstdint>

namespace engine::render {
namespace {

[[nodiscard]] float saturate1(float x) noexcept { return std::clamp(x, 0.0F, 1.0F); }

[[nodiscard]] float reinhard1(float x) noexcept { return saturate1(x / (1.0F + x)); }

// The Narkowicz fit. max(0) upstream makes the denominator's own zero unreachable for the inputs this
// is called with, but the max() here is kept: this function is not private and a future caller is not
// obliged to know that.
[[nodiscard]] float aces1(float x) noexcept {
    const float num = x * ((ACES_A * x) + ACES_B);
    const float den = (x * ((ACES_C * x) + ACES_D)) + ACES_E;
    return saturate1(num / std::max(den, 1e-6F));
}

}  // namespace

std::string_view tonemapOperatorLabel(TonemapOperator op) noexcept {
    // NO default: -- a fourth enumerator must be a -Wswitch error on the Linux lane, never a silent
    // fallthrough.
    switch (op) {
        case TonemapOperator::None:
            return "None";
        case TonemapOperator::Reinhard:
            return "Reinhard";
        case TonemapOperator::AcesApprox:
            return "ACES";
        case TonemapOperator::Count:
            break;
    }
    return "Unknown";  // Count and any out-of-range cast land here.
}

TonemapParams sanitizeTonemapParams(TonemapParams params) noexcept {
    if (std::isnan(params.exposure)) {
        params.exposure = 1.0F;  // NOT a clamp bound: NaN carries no direction.
    } else {
        params.exposure = std::clamp(params.exposure, MIN_EXPOSURE, MAX_EXPOSURE);
    }  // +inf clamps to MAX, -inf to MIN, both exactly.
    if (static_cast<std::uint8_t>(params.curve) >= static_cast<std::uint8_t>(TonemapOperator::Count)) {
        params.curve = TonemapOperator::AcesApprox;
    }
    return params;
}

float linearToSrgbEncode(float linear) noexcept {
    if (linear <= SRGB_ENCODE_THRESHOLD) {
        return SRGB_ENCODE_SLOPE * linear;
    }
    return (SRGB_ENCODE_SCALE * std::pow(linear, SRGB_ENCODE_GAMMA)) - SRGB_ENCODE_OFFSET;
}

Vec3 applyTonemapCurve(Vec3 exposed, TonemapOperator op) noexcept {
    switch (op) {  // NO default:
        case TonemapOperator::None:
            return Vec3{saturate1(exposed.x), saturate1(exposed.y), saturate1(exposed.z)};
        case TonemapOperator::Reinhard:
            return Vec3{reinhard1(exposed.x), reinhard1(exposed.y), reinhard1(exposed.z)};
        case TonemapOperator::AcesApprox:
        case TonemapOperator::Count:
            break;
    }
    return Vec3{aces1(exposed.x), aces1(exposed.y), aces1(exposed.z)};
}

Vec3 tonemapAndEncode(Vec3 linearHdr, const TonemapParams& sanitized) noexcept {
    // ORDER IS THE CONTRACT (TM19): exposure BEFORE the curve, min BEFORE max, saturate AFTER the
    // curve, OETF LAST and UNCONDITIONAL.
    const auto step = [&](float v) noexcept {
        return std::max(std::min(v * sanitized.exposure, TONEMAP_MAX_INPUT), 0.0F);
    };
    const Vec3 exposed{step(linearHdr.x), step(linearHdr.y), step(linearHdr.z)};
    const Vec3 mapped = applyTonemapCurve(exposed, sanitized.curve);
    return Vec3{linearToSrgbEncode(saturate1(mapped.x)), linearToSrgbEncode(saturate1(mapped.y)),
                linearToSrgbEncode(saturate1(mapped.z))};
}

}  // namespace engine::render
