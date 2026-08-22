#pragma once
// engine/render/src/material_pack.hpp — task 3.4.1. PRIVATE to engine/render (src/, never installed,
// never in the public include tree) — the primitives.hpp precedent, reached from tests/ by a relative
// include.
//
// The CPU mirrors of the TWO `space3` cbuffers in shaders/scene.frag.hlsl, and the two functions that
// fill them: `cbuffer Lights : register(b0, space3)` (400 bytes since task 3.6.2, pushed once per
// view) and
// `cbuffer MaterialParams : register(b1, space3)` (48 bytes, pushed on material change). Both live in
// a header rather than forward_renderer.cpp's anonymous namespace for exactly one reason: A FILE-LOCAL
// PACKER IS UNFALSIFIABLE. The "Opaque pushes cutoff 0.0" rule (AC-39) and the view vector's origin
// (eyePosition, the one CPU-side field the GGX rewrite added to the light block) are each a behaviour
// a tier-0 case must be able to see — and a seed that drops either one still records a frame and still
// renders a LIT image, merely a wrong one, which is precisely the "plausible garbage" class R5 names.
// Anything a packer decides belongs here; the renderer keeps the device calls.

#include <aero/core/math.hpp>
#include <aero/render/lighting.hpp>  // RenderView, CameraView, MAX_POINT_LIGHTS
#include <aero/render/material.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace engine::render::detail {

// b0, space3 — the 1.4.1 light set, unchanged in layout but for the appended eye position. FIELD ORDER
// MUST MATCH THE HLSL EXACTLY: a mismatch here neither fails to compile nor fails to submit.
struct GpuDirLight {
    Vec3 direction;
    float intensity = 0.0F;
    Vec3 color;
    float pad0 = 0.0F;
};
static_assert(sizeof(GpuDirLight) == 32);

struct GpuPointLight {
    Vec3 position;
    float range = 0.0F;
    Vec3 color;
    float intensity = 0.0F;
};
static_assert(sizeof(GpuPointLight) == 32);

struct GpuLightBlock {
    Vec3 ambient;
    std::uint32_t pointCount = 0;
    GpuDirLight dir;
    std::array<GpuPointLight, MAX_POINT_LIGHTS> points{};
    // task 3.4.1 — the one field the GGX BRDF needs that Lambert did not. CameraView has carried
    // eyePosition unused since 1.4.1, whose own comment says it exists "for future specular/fresnel
    // terms" (lighting.hpp). Appended AFTER the existing members, so every pre-3.4.1 field keeps its
    // offset and the growth is invisible to anything that does not read the tail.
    Vec3 eyePosition;
    float pad0 = 0.0F;
    // task 3.6.2 — the per-VIEW shadow data, APPENDED after eyePosition exactly as eyePosition was
    // appended after `points` at 3.4.1: every pre-3.6.2 field keeps its offset, so the growth is
    // invisible to anything that does not read the tail.
    Mat4 lightViewProj;  // shadowViewProj(fit); identity when the view carries no valid ShadowView
    Vec4 shadowParams;   // x texelSize (1/resolution), y constantBias, z normalBias, w enabled?1:0
};
static_assert(sizeof(GpuLightBlock) == 16 + 32 + (32 * 8) + 16 + 64 + 16);  // 400 (was 320)
static_assert(std::is_trivially_copyable_v<GpuLightBlock>);
// task 3.6.2 (INV-5) -- the layout is PINNED, not merely SIZED. Swapping the two appended fields
// keeps sizeof at 400 and keeps every earlier field's offset, so nothing that reads the block could
// tell -- while the HLSL would then disagree with the C++ silently, which is exactly the class this
// header's own opening rule warns about ("a mismatch here neither fails to compile nor fails to
// submit"). These two lines are the closure.
static_assert(offsetof(GpuLightBlock, lightViewProj) == 320);
static_assert(offsetof(GpuLightBlock, shadowParams) == 384);

// THE one place a RenderView becomes the bytes b0 receives. Zero-initialized first, so the unused tail
// of `points` is deterministic rather than whatever the stack held, and the point count is CLAMPED
// here as well as at the bridge — draw() must not read past the array for a view assembled by hand.
[[nodiscard]] inline GpuLightBlock packLights(const RenderView& view) noexcept {
    GpuLightBlock block{};
    block.ambient = view.ambient;
    block.dir = {view.directional.direction, view.directional.intensity, view.directional.color, 0.0F};
    const std::size_t count = std::min<std::size_t>(view.points.size(), MAX_POINT_LIGHTS);
    for (std::size_t i = 0; i < count; ++i) {
        const PointLightData& src = view.points[i];
        block.points[i] = {src.position, src.range, src.color, src.intensity};
    }
    block.pointCount = static_cast<std::uint32_t>(count);
    block.eyePosition = view.camera.eyePosition;  // task 3.4.1 — the BRDF's view vector origin
    // task 3.6.2 (D5) — straight off the VIEW, never off a renderer member. An unassigned or
    // invalid ShadowView writes an identity matrix and w == 0, which the shader reads as "shade
    // unshadowed" through its ONE branch; there is no #if, no second variant and no branch on a
    // texture handle. A caller who never calls renderShadowMap lands here, and that is a legitimate
    // opt-out rather than a defect, so nothing warns.
    block.lightViewProj = view.shadow.valid ? view.shadow.lightViewProj : Mat4::identity();
    block.shadowParams = view.shadow.valid
                             ? Vec4{view.shadow.texelSize, view.shadow.constantBias, view.shadow.normalBias, 1.0F}
                             : Vec4{};
    return block;
}

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
