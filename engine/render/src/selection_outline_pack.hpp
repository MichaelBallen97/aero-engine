#pragma once
// engine/render/src/selection_outline_pack.hpp — task E.1.4. PRIVATE to engine/render (src/, never
// installed, never in the public include tree) — the tonemap_pack.hpp / material_pack.hpp /
// skinning_pack.hpp precedent, reached from tests/ by a relative include.
//
// A FILE-LOCAL PACKER IS UNFALSIFIABLE (3.4.1's rule): a packer that swaps the two colours, or the
// two float2s, still records a frame and still draws a picture -- merely a wrong one, which is
// precisely the "plausible garbage" class.
//
// The CPU mirror of the ONE fragment cbuffer in the outline pair, under the 0.4.3 binding law
// (fragment textures/samplers -> space2, fragment UBOs -> space3):
//
//   shaders/selection_outline.frag.hlsl : cbuffer SelectionOutlineParams : register(b0, space3)
//        float4 uPrimaryColor; float4 uSecondaryColor; float2 uTexelStep; float2 uUvClampMax;  // 48 B
//
// THE VERTEX BLOCK IS detail::packTonemapVertex, REUSED, NEVER RESPELLED. fullscreen.vert.hlsl's
// FullscreenParams has exactly one packer in this tree; a second, byte-identical one here would
// create a drift surface with no benefit -- the two would be free to disagree, and the disagreement
// would be a vertically-flipped or scaled overlay that no tier-0 case would see. SO6 pins the reuse.

#include <aero/core/math.hpp>
#include <aero/render/selection_outline.hpp>
#include <aero/rhi/types.hpp>  // rhi::Extent2D -- selectionOutlineClampUvMax's two arguments

#include <algorithm>  // std::min -- the drawn extent is clamped to the allocation first
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>  // std::memcpy -- MSVC's STL does not supply <cstring> transitively
#include <type_traits>

namespace engine::render::detail {

// std140. Two float4s, then two float2s: 16 + 16 + 8 + 8 = 48 bytes, and the last member ends on a
// 16-byte boundary so no tail padding is needed. FIELD ORDER MUST MATCH selection_outline.frag.hlsl's
// cbuffer EXACTLY -- a mismatch here neither fails to compile nor fails to submit.
inline constexpr std::size_t SELECTION_OUTLINE_FRAGMENT_UNIFORM_BYTES = 48;

static_assert(sizeof(float) == 4);
static_assert(std::is_trivially_copyable_v<float>);

// THE FRAGMENT STAGE'S CLAMP BOUND, and it is DELIBERATELY NOT tonemapSourceUvMax. That function
// returns the drawn sub-rect's EXCLUSIVE far edge, drawExtent / textureExtent, which is exactly right
// for the VERTEX stage -- it is the uv the fullscreen triangle's far corner interpolates to, and
// packTonemapVertex still receives it unchanged.
//
// As a CLAMP bound it is off by HALF A TEXEL and draws a false band. Under Nearest filtering the texel
// a uv names is floor(uv * extent), and floor((drawW / texW) * texW) == drawW -- the FIRST CLEARED
// MARGIN texel, not the last drawn one at drawW - 1. A tap clamped there reads 0 against a silhouette
// of 1, mn < mx holds, and a band is drawn along the frame edge: the exact picture D9 says cannot
// happen. The defect is ASYMMETRIC -- the low bound clamps to 0, which IS a drawn texel -- so it shows
// on the right and bottom edges only, and it is INVISIBLE whenever drawExtent == textureExtent, i.e.
// in every target created with quantum = 1. Measured on the editor's own pair (draw 200x140,
// allocation 256x192): 140 of 140 rows band on the right edge and 200 of 200 columns on the bottom,
// against 0 of each at quantum = 1.
//
// The bound is therefore the LAST DRAWN TEXEL'S CENTRE, (drawExtent - 0.5) / textureExtent, which
// names texel drawExtent - 1 on both axes. Its two degenerate arms are tonemapSourceUvMax's own, for
// its own reasons: a zero textureExtent is a not-renderable target and answers 1.0 rather than
// dividing by zero, and a drawExtent past the allocation is clamped to the allocation first, so the
// bound never leaves the texture. A zero drawExtent answers 0.0 -- nothing is drawn, and every tap
// collapses onto texel 0.
[[nodiscard]] inline Vec2 selectionOutlineClampUvMax(rhi::Extent2D drawExtent, rhi::Extent2D textureExtent) noexcept {
    const auto axis = [](std::uint32_t draw, std::uint32_t texture) {
        if (texture == 0) {
            return 1.0F;  // a not-renderable target: nothing is drawn from it anyway
        }
        const std::uint32_t drawn = std::min(draw, texture);
        if (drawn == 0) {
            return 0.0F;
        }
        return (static_cast<float>(drawn) - 0.5F) / static_cast<float>(texture);
    };
    return Vec2{axis(drawExtent.width, textureExtent.width), axis(drawExtent.height, textureExtent.height)};
}

// b0, space3 -- {primary.rgba, secondary.rgba, step.xy, clampUvMax.xy}. THE WHOLE BUFFER IS ZEROED
// FIRST, so no byte is ever indeterminate: SDL copies the block verbatim into its uniform ring, and
// an indeterminate tail byte is a value that differs between runs on the same machine.
//
// The last field is selectionOutlineClampUvMax's answer and NOT tonemapSourceUvMax's -- the two are
// half a texel apart and the difference is a band along the frame edge. It REPLACES the exclusive
// uvMax rather than sitting beside it: the fragment stage has exactly one use for a far-corner uv,
// the tap clamp, and a second field it never reads would be a drift surface with no consumer.
//
// `sanitized` MUST have been through sanitizeSelectionOutlineParams: this function does not sanitize,
// and SO5 pins that the contract is the caller's (composite() is the one production caller, and it
// does sanitize).
[[nodiscard]] inline std::array<std::byte, SELECTION_OUTLINE_FRAGMENT_UNIFORM_BYTES> packSelectionOutlineFragment(
    const SelectionOutlineParams& sanitized, Vec2 texelStep, Vec2 clampUvMax) noexcept {
    std::array<std::byte, SELECTION_OUTLINE_FRAGMENT_UNIFORM_BYTES> out{};  // value-init: every byte zero
    std::memcpy(out.data() + 0, &sanitized.primaryColorSrgb.x, sizeof(float));
    std::memcpy(out.data() + 4, &sanitized.primaryColorSrgb.y, sizeof(float));
    std::memcpy(out.data() + 8, &sanitized.primaryColorSrgb.z, sizeof(float));
    std::memcpy(out.data() + 12, &sanitized.primaryColorSrgb.w, sizeof(float));
    std::memcpy(out.data() + 16, &sanitized.secondaryColorSrgb.x, sizeof(float));
    std::memcpy(out.data() + 20, &sanitized.secondaryColorSrgb.y, sizeof(float));
    std::memcpy(out.data() + 24, &sanitized.secondaryColorSrgb.z, sizeof(float));
    std::memcpy(out.data() + 28, &sanitized.secondaryColorSrgb.w, sizeof(float));
    std::memcpy(out.data() + 32, &texelStep.x, sizeof(float));
    std::memcpy(out.data() + 36, &texelStep.y, sizeof(float));
    std::memcpy(out.data() + 40, &clampUvMax.x, sizeof(float));
    std::memcpy(out.data() + 44, &clampUvMax.y, sizeof(float));
    return out;
}

}  // namespace engine::render::detail
