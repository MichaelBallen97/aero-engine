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
//        float4 uPrimaryColor; float4 uSecondaryColor; float2 uTexelStep; float2 uUvMax;  // 48 bytes
//
// THE VERTEX BLOCK IS detail::packTonemapVertex, REUSED, NEVER RESPELLED. fullscreen.vert.hlsl's
// FullscreenParams has exactly one packer in this tree; a second, byte-identical one here would
// create a drift surface with no benefit -- the two would be free to disagree, and the disagreement
// would be a vertically-flipped or scaled overlay that no tier-0 case would see. SO6 pins the reuse.

#include <aero/core/math.hpp>
#include <aero/render/selection_outline.hpp>

#include <array>
#include <cstddef>
#include <cstring>  // std::memcpy -- MSVC's STL does not supply <cstring> transitively
#include <type_traits>

namespace engine::render::detail {

// std140. Two float4s, then two float2s: 16 + 16 + 8 + 8 = 48 bytes, and the last member ends on a
// 16-byte boundary so no tail padding is needed. FIELD ORDER MUST MATCH selection_outline.frag.hlsl's
// cbuffer EXACTLY -- a mismatch here neither fails to compile nor fails to submit.
inline constexpr std::size_t SELECTION_OUTLINE_FRAGMENT_UNIFORM_BYTES = 48;

static_assert(sizeof(float) == 4);
static_assert(std::is_trivially_copyable_v<float>);

// b0, space3 -- {primary.rgba, secondary.rgba, step.xy, uvMax.xy}. THE WHOLE BUFFER IS ZEROED FIRST,
// so no byte is ever indeterminate: SDL copies the block verbatim into its uniform ring, and an
// indeterminate tail byte is a value that differs between runs on the same machine.
//
// `sanitized` MUST have been through sanitizeSelectionOutlineParams: this function does not sanitize,
// and SO5 pins that the contract is the caller's (composite() is the one production caller, and it
// does sanitize).
[[nodiscard]] inline std::array<std::byte, SELECTION_OUTLINE_FRAGMENT_UNIFORM_BYTES> packSelectionOutlineFragment(
    const SelectionOutlineParams& sanitized, Vec2 texelStep, Vec2 uvMax) noexcept {
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
    std::memcpy(out.data() + 40, &uvMax.x, sizeof(float));
    std::memcpy(out.data() + 44, &uvMax.y, sizeof(float));
    return out;
}

}  // namespace engine::render::detail
