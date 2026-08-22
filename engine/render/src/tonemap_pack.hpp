#pragma once
// engine/render/src/tonemap_pack.hpp — task 3.6.3. PRIVATE to engine/render (src/, never installed,
// never in the public include tree) — the material_pack.hpp / skinning_pack.hpp / primitives.hpp
// precedent, reached from tests/ by a relative include.
//
// A FILE-LOCAL PACKER IS UNFALSIFIABLE (3.4.1's rule): a packer that swaps the two blocks, writes the
// curve as a float, or leaves a padding byte indeterminate still records a frame and still draws a
// picture -- merely a wrong one, which is precisely the "plausible garbage" class.
//
// The CPU mirrors of the TWO cbuffers in the tonemap pair, under the 0.4.3 binding law
// (vertex t/s -> space0, vertex UBOs -> space1; fragment t/s -> space2, fragment UBOs -> space3):
//
//   shaders/fullscreen.vert.hlsl : cbuffer FullscreenParams : register(b0, space1)
//        float2 uUvScale; float2 _pad0;                                   // 16 bytes
//   shaders/tonemap.frag.hlsl    : cbuffer TonemapParams : register(b0, space3)
//        float uExposure; uint uCurve; float2 _pad0;                      // 16 bytes
//
// FIELD ORDER MUST MATCH THE HLSL EXACTLY: a mismatch here neither fails to compile nor fails to
// submit.

#include <aero/core/math.hpp>
#include <aero/render/tonemap.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>  // std::memcpy -- MSVC's STL does not supply <cstring> transitively
#include <type_traits>

namespace engine::render::detail {

// THE ENUM MIRROR. tonemap.frag.hlsl compares the RAW INTEGER uCurve against literal 1 and 2, so
// TonemapOperator's underlying values are part of the wire contract, not an implementation detail
// (INV-5). Renumbering the enum without editing the HLSL silently swaps two curves.
static_assert(static_cast<std::uint8_t>(TonemapOperator::None) == 0);
static_assert(static_cast<std::uint8_t>(TonemapOperator::Reinhard) == 1);
static_assert(static_cast<std::uint8_t>(TonemapOperator::AcesApprox) == 2);
static_assert(static_cast<std::uint8_t>(TonemapOperator::Count) == 3);
static_assert(TONEMAP_OPERATOR_COUNT == static_cast<std::size_t>(TonemapOperator::Count));

// std140: a float2 plus padding, and a float + uint plus padding. Both blocks are 16 bytes, which is
// also SDL_GPU's minimum useful push granularity.
inline constexpr std::size_t TONEMAP_VERTEX_UNIFORM_BYTES = 16;
inline constexpr std::size_t TONEMAP_FRAGMENT_UNIFORM_BYTES = 16;

static_assert(sizeof(float) == 4 && sizeof(std::uint32_t) == 4);
static_assert(std::is_trivially_copyable_v<float> && std::is_trivially_copyable_v<std::uint32_t>);

// b0, space1 -- {uUvScale.x, uUvScale.y, 0, 0}. THE WHOLE BUFFER IS ZEROED FIRST, so no padding byte
// is ever indeterminate: SDL copies the block verbatim into its uniform ring, and an indeterminate
// tail byte is a value that differs between runs on the same machine.
[[nodiscard]] inline std::array<std::byte, TONEMAP_VERTEX_UNIFORM_BYTES> packTonemapVertex(Vec2 uvScale) noexcept {
    std::array<std::byte, TONEMAP_VERTEX_UNIFORM_BYTES> out{};  // value-init: every byte zero
    std::memcpy(out.data() + 0, &uvScale.x, sizeof(float));
    std::memcpy(out.data() + 4, &uvScale.y, sizeof(float));
    return out;
}

// b0, space3 -- {uExposure, uCurve, 0, 0}. `sanitized` MUST have been through sanitizeTonemapParams:
// this function does not sanitize, and TM27 pins that the contract is the caller's
// (PostProcess::resolve is the one production caller and it does sanitize).
[[nodiscard]] inline std::array<std::byte, TONEMAP_FRAGMENT_UNIFORM_BYTES> packTonemapFragment(
    const TonemapParams& sanitized) noexcept {
    std::array<std::byte, TONEMAP_FRAGMENT_UNIFORM_BYTES> out{};
    const float exposure = sanitized.exposure;
    const auto curve = static_cast<std::uint32_t>(sanitized.curve);
    std::memcpy(out.data() + 0, &exposure, sizeof(float));
    std::memcpy(out.data() + 4, &curve, sizeof(std::uint32_t));
    return out;
}

}  // namespace engine::render::detail
