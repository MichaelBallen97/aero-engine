#pragma once
// engine/render/src/material_pack.hpp — task 3.4.1. PRIVATE to engine/render (src/, never installed,
// never in the public include tree) — the primitives.hpp precedent, reached from tests/ by a relative
// include.
//
// The CPU mirror of shaders/scene.frag.hlsl's `cbuffer MaterialParams : register(b1, space3)` and the
// one function that fills it. It lives in a header rather than forward_renderer.cpp's anonymous
// namespace for exactly one reason: the "Opaque pushes cutoff 0.0" rule (AC-39) is a behaviour a
// tier-0 case must be able to see, and a file-local function is unfalsifiable — the sabotage seed that
// pushes the file's cutoff for an Opaque material would redden nothing at all.

#include <aero/core/math.hpp>
#include <aero/render/material.hpp>

#include <type_traits>

namespace engine::render::detail {

// b1, space3 — 48 bytes, 16-byte aligned throughout, no straddle. FIELD ORDER MUST MATCH THE HLSL
// EXACTLY (D11): float4 then float3+float then four scalars, which HLSL packs into three 16-byte
// registers and C++ lays out identically. The sizeof static_assert is the tripwire against a silent
// packing drift; a mismatch here does not fail to compile or submit, it renders plausible garbage.
struct GpuMaterialParams {
    Vec4 baseColorFactor;
    Vec3 emissiveFactor;
    float normalScale = 0.0F;
    float metallicFactor = 0.0F;
    float roughnessFactor = 0.0F;
    float occlusionStrength = 0.0F;
    float alphaCutoff = 0.0F;
};
static_assert(sizeof(GpuMaterialParams) == 48);
static_assert(std::is_trivially_copyable_v<GpuMaterialParams>);

// THE one place the pushed alpha cutoff is decided (AC-39). Mask pushes the material's own cutoff;
// Opaque AND Blend push 0.0, which a texel's alpha can never fall below, so the shader's single
// `if (baseColor.a < uAlphaCutoff) discard;` can never fire for them — no sentinel value, no second
// shader variant, and no branch the GPU has to predict. Blend rides the Opaque arm deliberately: it
// is DRAWN opaque in v1 (D9), so it must not discard either.
[[nodiscard]] inline GpuMaterialParams packMaterial(const MaterialParams& params) noexcept {
    GpuMaterialParams gpu{};
    gpu.baseColorFactor = params.baseColorFactor;
    gpu.emissiveFactor = params.emissiveFactor;
    gpu.normalScale = params.normalScale;
    gpu.metallicFactor = params.metallicFactor;
    gpu.roughnessFactor = params.roughnessFactor;
    gpu.occlusionStrength = params.occlusionStrength;
    gpu.alphaCutoff = params.alpha == MaterialAlpha::Mask ? params.alphaCutoff : 0.0F;
    return gpu;
}

}  // namespace engine::render::detail
