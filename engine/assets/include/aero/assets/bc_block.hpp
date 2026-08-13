#pragma once
// Aero Engine — the BC1 and BC4 block encoders (task 3.3.2). PURE, INTEGER-ONLY, and public rather
// than src-private so aero_tests can drive them directly against per-block byte goldens -- which is
// where their coverage belongs. An encoder tested only through a whole-image cook is tested through a
// filter, a block loop and an assembler, and a per-block regression then shows up as a diff in a
// 400-byte array rather than as a failing assertion that names the block.
//
// BC3 AND BC5 HAVE NO ENCODERS AND MAY NEVER GROW ONE:
//   BC3 = encodeBc4Block(alpha) into bytes 0..7, then encodeBc1Block(rgb) into bytes 8..15
//   BC5 = encodeBc4Block(red)   into bytes 0..7, then encodeBc4Block(green) into bytes 8..15
// Both composition ORDERS are output-byte decisions with their own tests, because swapping either
// produces a plausible image rather than an obviously broken one.
//
// NO FLOATING POINT. stb_dxt.h ships with the stb port this tree already depends on and provides
// exactly these four formats -- and is not used, because its BC1 colour path finds the principal axis
// by float power iteration (stb_dxt.h:323-325), which is an FMA-contraction target: clang on arm64
// contracts by default and MSVC under /fp:precise does not, so the same input can produce different
// bytes on two of this project's three CI lanes. Task 3.3.3 turns cross-platform byte-identity into a
// CI job for both cook kinds. That is the whole argument; it is about determinism, not quality. The
// accepted, stated cost is that a first-party BC1 is a little worse than a mature one, and by how
// much is a MEASUREMENT on the validation page rather than a guess here.
//
// stb is also a vcpkg package, and engine/assets links none at all -- which is what makes that
// target's PRIVATE links a real compile-time boundary. So the placement and the determinism argument
// point the same way.
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::assets {

// The frozen per-channel weights of the integer squared error the index assignment minimizes,
// approximating luma sensitivity. THEY CHANGE OUTPUT BYTES, so they are a frozen quality decision and
// not a tunable: moving them is a COOKED_TEXTURE_COOKER_VERSION bump.
inline constexpr std::int32_t BC1_ERROR_WEIGHT_R = 3;
inline constexpr std::int32_t BC1_ERROR_WEIGHT_G = 6;
inline constexpr std::int32_t BC1_ERROR_WEIGHT_B = 1;

// FIXED at two, deliberately. An error-driven stopping rule ("iterate until the error stops
// improving") would be a second determinism surface -- one whose behaviour depends on an epsilon --
// for no measurable gain.
inline constexpr std::int32_t BC1_REFINEMENT_ITERATIONS = 2;

// `srcRgba` is 16 RGBA8 texels in ROW-MAJOR order, texel (x, y) at 4 * (4 * y + x). The caller has
// ALREADY CLAMPED edge texels -- the encoder sees 16 valid texels, always -- so a partial edge block
// is the block loop's problem and never this function's. ALPHA IS IGNORED:
// VK_FORMAT_BC1_RGB_* has none. Writes exactly 8 bytes and touches nothing else.
//
// The fixed span extents are deliberate: they make a wrongly-sized call a COMPILE error rather than a
// runtime read, and they cost nothing.
void encodeBc1Block(std::span<const std::uint8_t, 64> srcRgba, std::span<std::byte, 8> dst) noexcept;

// `src` is 16 single-channel values in ROW-MAJOR order. Writes exactly 8 bytes.
void encodeBc4Block(std::span<const std::uint8_t, 16> src, std::span<std::byte, 8> dst) noexcept;

}  // namespace engine::assets
