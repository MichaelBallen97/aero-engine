#pragma once
// Aero Engine — render tonemap vocabulary (task 3.6.3). PUBLIC and PURE: no rhi type, no allocation,
// no logging, no I/O, nothing that can throw -- the culling.hpp posture, and the third pure module in
// this layer after animation.hpp and culling.hpp.
//
// THIS FILE IS THE SOURCE shaders/tonemap.frag.hlsl TRANSCRIBES. The HLSL is a second copy of the
// same arithmetic, in a language no test in this tree can execute, so TM29 pins the constants and the
// arm order as SOURCE TEXT. Change one side and you must change the other; TM29 is what says so out
// loud.
//
// THREE PROPERTIES STATED HERE BECAUSE THEY LOOK LIKE DEFECTS AND ARE NOT:
//
//  1. TonemapOperator::None means "no tone CURVE". Exposure is still applied, the result is still
//     saturated to [0,1], and the sRGB OETF is still applied. There is deliberately no applyOetf
//     flag, no gammaEnabled bool and no linearOutput mode: a flag that can be set wrong is a way to
//     ship the exact defect this task exists to remove. `None` is an ISOLATION CONTROL -- it
//     separates "the curve looks wrong" from "the encode is missing". The only way to get an
//     un-encoded picture is to not use PostProcess at all (samples/phase-3-tonemap --raw).
//
//  2. The Narkowicz ACES fit is NOT near-identity in the shadows. Its slope at the origin is
//     ACES_B / ACES_E = 0.03 / 0.14 ~= 0.2143, so a linear 0.02 maps to ~0.0105, not ~0.02. That is a
//     genuine characteristic of this fit, not a defect, and it is the reason `exposure` exists as a
//     knob. TM20 pins the origin slope against that literal so a future "the shadows look crushed,
//     let me tweak a constant" edit reddens a test instead of silently becoming a different curve.
//     DO NOT "FIX" IT. If a validation pass judges the default too dark, the fix is the default
//     EXPOSURE, recorded as an amendment -- never a silent constant edit.
//
//  3. NaN IS A DECLARED NON-CHECK ON BOTH SIDES, the way docs/09 section 13.10 declares its own, and
//     the C++ side is named here explicitly because it is easy to read the shader-only wording this
//     note used to carry as a promise that the CPU function is different. IT IS NOT.
//     tonemapAndEncode(NaN) RETURNS NaN: min/max/clamp are specified in terms of `<`, which is false
//     in both directions for NaN, so each returns its NaN operand, and std::pow(NaN, y) is NaN. That
//     is portable rather than accidental, and it is deliberate -- HLSL's min/max with NaN are
//     implementation-defined, so a CPU-side NaN mapping would make this function a DIFFERENT function
//     from the one shaders/tonemap.frag.hlsl transcribes, which is exactly what TM29 exists to
//     prevent. A NaN pixel is an upstream defect (3.5.2's code-review round found and closed the one
//     known producer) and this chain propagates it visibly rather than hiding it.
//     WHAT IS PROMISED: a FINITE output for every FINITE input and for +-inf, on every operator; and
//     sanitizeTonemapParams guarantees the UNIFORM is never NaN, which is the half this layer can
//     promise unconditionally. A CALLER THAT NARROWS THE RESULT TO AN INTEGER MUST GUARD FIRST --
//     std::lround of a NaN is unspecified and may raise FE_INVALID (samples/phase-3-tonemap does).

#include <aero/core/math.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace engine::render {

// The values are EXPLICIT because they are a wire format: tonemap.frag.hlsl compares the raw integer
// uCurve against literal 1 and 2 (INV-5). tonemap_pack.hpp's static_assert block is the mirror; TM1
// is the test.
enum class TonemapOperator : std::uint8_t { None = 0, Reinhard = 1, AcesApprox = 2, Count = 3 };

inline constexpr std::size_t TONEMAP_OPERATOR_COUNT = 3;

// Powers of two, so both bounds are exactly representable and a clamp to either is exact.
inline constexpr float MIN_EXPOSURE = 0.015625F;  // 2^-6
inline constexpr float MAX_EXPOSURE = 64.0F;      // 2^6

// The largest finite half. The HDR buffer is RGBA16Float, so this value is exactly representable in
// the thing being read, and clamping to it is a no-op for every value the buffer can actually hold
// EXCEPT +inf -- which is reachable from a large emissiveFactor (material.hpp's emissiveFactor is an
// unclamped Vec3 and material_pack.hpp does not clamp it either). Feeding +inf to the ACES rational
// form gives inf/inf = NaN, which is a hot pink or a black pixel depending on the backend. At 65504
// the ACES numerator is ~1.077e10 and the denominator ~1.042e10 -- both comfortably finite in fp32 --
// and the ratio saturates to 1.0.
inline constexpr float TONEMAP_MAX_INPUT = 65504.0F;

// Narkowicz RRT+ODT fit, the five-constant rational form. Named so TM29's source-text pin has a C++
// side to compare against, and so TM18 can assert them by value.
inline constexpr float ACES_A = 2.51F;
inline constexpr float ACES_B = 0.03F;
inline constexpr float ACES_C = 2.43F;
inline constexpr float ACES_D = 0.59F;
inline constexpr float ACES_E = 0.14F;

// sRGB OETF (IEC 61966-2-1). THE THRESHOLD IS THE ENCODE ONE (0.0031308), NEVER THE DECODE ONE
// (0.04045) -- swapping them is the single most common defect in this function, and the two are
// confusable precisely because 12.92 * 0.0031308 ~= 0.04045. TM8 pins both by value.
inline constexpr float SRGB_ENCODE_THRESHOLD = 0.0031308F;
inline constexpr float SRGB_ENCODE_SLOPE = 12.92F;
inline constexpr float SRGB_ENCODE_SCALE = 1.055F;
inline constexpr float SRGB_ENCODE_OFFSET = 0.055F;
inline constexpr float SRGB_ENCODE_GAMMA = 1.0F / 2.4F;

// Exposure and a curve, and NOTHING ELSE (see property 1 above).
struct TonemapParams {
    float exposure = 1.0F;
    TonemapOperator curve = TonemapOperator::AcesApprox;
    bool operator==(const TonemapParams&) const = default;
};

// Total over the enum, distinct, never empty. NEVER named toString: doctest's DOCTEST_STRINGIFY
// expands to an UNQUALIFIED toString(...), which ADL would find here, and the decomposer then tries
// std::string_view + const char* -- a hard compile error on every lane, reported inside doctest.h.
// See .claude/rules/ci-portability.md; cookedTextureStatusLabel and 3.4.1's four material*Label
// functions are named the way they are for this exact reason.
[[nodiscard]] std::string_view tonemapOperatorLabel(TonemapOperator op) noexcept;

// TOTAL. NaN exposure -> exactly 1.0F; +inf -> MAX_EXPOSURE; -inf -> MIN_EXPOSURE; otherwise clamped
// to [MIN_EXPOSURE, MAX_EXPOSURE]. A curve whose underlying value is >= Count -> AcesApprox.
// INV-3 rests on this: the packer's guarantee that no NaN or infinity reaches a uniform is this
// function's guarantee, and PostProcess::resolve calls it before packing anything.
[[nodiscard]] TonemapParams sanitizeTonemapParams(TonemapParams params) noexcept;

// The sRGB OETF, piecewise. `linear` is assumed already in [0,1] -- the caller saturates.
[[nodiscard]] float linearToSrgbEncode(float linear) noexcept;

// Per channel. None is the identity, Reinhard is x/(1+x), AcesApprox is the rational fit; each arm
// saturates its own result.
[[nodiscard]] Vec3 applyTonemapCurve(Vec3 exposed, TonemapOperator op) noexcept;

// THE CHAIN, IN ONE FUNCTION -- the definition tonemap.frag.hlsl transcribes:
//   x * exposure  ->  min(TONEMAP_MAX_INPUT)  ->  max(0)  ->  curve  ->  saturate  ->  OETF
// `params` must ALREADY BE SANITIZED (sanitizeTonemapParams); passing an unsanitized value is a
// caller bug, and TM27 pins that the contract is the caller's rather than this function's.
// FINITE for every FINITE `linearHdr` and for +-inf; PROPAGATES a NaN channel (property 3).
[[nodiscard]] Vec3 tonemapAndEncode(Vec3 linearHdr, const TonemapParams& sanitized) noexcept;

}  // namespace engine::render
