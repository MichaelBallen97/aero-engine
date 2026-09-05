#pragma once
// engine/render/src/sky_pack.hpp — task E.2.1. PRIVATE to engine/render (src/, never installed,
// never in the public include tree) — the material_pack.hpp / tonemap_pack.hpp precedent, reached
// from tests/ by a relative include.
//
// A FILE-LOCAL PACKER IS UNFALSIFIABLE (3.4.1's rule): a packer that transposes the inverse, swaps
// two colour rows or leaves a padding float indeterminate still records a frame and still draws a
// picture -- merely a wrong one, which is precisely the "plausible garbage" class.
//
// The CPU mirrors of the TWO cbuffers in the sky pair, under the 0.4.3 binding law (vertex UBOs ->
// space1, fragment UBOs -> space3):
//
//   shaders/sky.vert.hlsl : cbuffer SkyCamera  : register(b0, space1)
//        float4x4 uInvViewProj;                                            // 64 bytes
//   shaders/sky.frag.hlsl : cbuffer SkyParams  : register(b0, space3)
//        float3 uHorizon; float _pad0; float3 uSkyDelta; float _pad1;
//        float3 uGroundDelta; float _pad2;                                 // 48 bytes
//
// FIELD ORDER MUST MATCH THE HLSL EXACTLY: a mismatch here neither fails to compile nor fails to
// submit.

#include <aero/core/math.hpp>
#include <aero/render/environment.hpp>  // SkyGradient
#include <aero/render/lighting.hpp>     // CameraView

#include <cmath>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace engine::render::detail {

// b0, space1 -- the vertex stage's camera. 64 bytes. Packed EXACTLY as GpuLightBlock::lightViewProj
// is and consumed as mul(uInvViewProj, float4(...)) exactly as uLightViewProj is (scene.frag.hlsl:86).
struct GpuSkyCamera {
    Mat4 invViewProj;
};
static_assert(sizeof(GpuSkyCamera) == 64);
static_assert(std::is_trivially_copyable_v<GpuSkyCamera>);

// b0, space3 -- the fragment stage's gradient. 48 bytes; three float3 + pad rows, no straddle.
struct GpuSkyParams {
    Vec3 horizon;
    float pad0 = 0.0F;
    Vec3 skyDelta;
    float pad1 = 0.0F;
    Vec3 groundDelta;
    float pad2 = 0.0F;
};
static_assert(sizeof(GpuSkyParams) == 48);
// The layout is PINNED, not merely SIZED (material_pack.hpp's INV-5 closure, same reasoning):
// permuting the three colour rows keeps sizeof at 48 and nothing that reads the block could tell,
// while the HLSL would disagree with the C++ silently.
static_assert(offsetof(GpuSkyParams, skyDelta) == 16);
static_assert(offsetof(GpuSkyParams, groundDelta) == 32);
static_assert(std::is_trivially_copyable_v<GpuSkyParams>);

// nullopt when inverse(proj * view) has ANY non-finite element -- a singular camera (engine::inverse
// yields inf/NaN for one, scene/camera.hpp:57's own note) or a non-finite input. All SIXTEEN elements
// are checked, culling.cpp's Frustum::valid idiom: a viewProj with an infinite translation column
// yields finite leading elements and an infinite last one, and checking a subset would pass it.
// A NEARLY singular camera yields a finite, WRONG matrix and a wrong sky, which is out of contract
// exactly as culling's degenerate-frustum posture states for its own extraction.
[[nodiscard]] inline std::optional<GpuSkyCamera> packSkyCamera(const CameraView& camera) noexcept {
    const Mat4 invViewProj = inverse(camera.proj * camera.view);
    for (const Vec4& column : invViewProj.columns) {
        if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) ||
            !std::isfinite(column.w)) {
            return std::nullopt;
        }
    }
    return GpuSkyCamera{.invViewProj = invViewProj};
}

// Pads are WRITTEN ZERO, never left to the stack (packLights's rule): SDL copies the block verbatim
// into its uniform ring, and an indeterminate tail float is a value that differs between runs on the
// same machine.
[[nodiscard]] inline GpuSkyParams packSkyParams(const SkyGradient& gradient) noexcept {
    GpuSkyParams params{};  // value-init: every pad is a written zero before anything else happens
    params.horizon = gradient.horizon;
    params.skyDelta = gradient.skyDelta;
    params.groundDelta = gradient.groundDelta;
    return params;
}

}  // namespace engine::render::detail
