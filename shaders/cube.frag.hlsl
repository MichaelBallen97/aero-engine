// Task 0.5.2 — textured cube fragment shader. Samples the cube texture and tints by the per-face color
// (Phase 0 has no lighting; the tint distinguishes faces). Binding law: fragment t/s -> space2.
Texture2D    uTexture : register(t0, space2);
SamplerState uSampler : register(s0, space2);
float4 main(float2 uv : TEXCOORD0, float3 color : TEXCOORD1) : SV_Target0 {
    const float3 texel = uTexture.Sample(uSampler, uv).rgb;
    return float4(texel * color, 1.0);
}
