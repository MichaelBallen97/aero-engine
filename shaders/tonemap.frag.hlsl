// Task 3.6.3 — the tonemap/gamma resolve. THIS IS A TRANSCRIPTION OF engine/render/tonemap.hpp:
// every constant below is a bare literal precisely so TM29's comment-stripped source-text pin can
// find it, and the C++ side is where the reasoning lives. Change one, change the other.
// Binding law (0.4.3 F8): fragment textures/samplers -> space2, fragment uniform buffers -> space3.
cbuffer TonemapParams : register(b0, space3) {
    float uExposure;
    uint  uCurve;       // the RAW TonemapOperator integer: 0 None, 1 Reinhard, 2 AcesApprox (INV-5)
    float2 _pad0;
};

Texture2D    uSceneTex : register(t0, space2);
SamplerState uSceneSmp : register(s0, space2);

float3 tonemapReinhard(float3 x) { return saturate(x / (1.0 + x)); }

// Narkowicz RRT+ODT fit. NOT near-identity in the shadows (slope 0.03/0.14 ~= 0.2143 at the origin);
// that is the fit's own character, not a defect -- see tonemap.hpp. Do not tweak these five.
float3 tonemapAcesApprox(float3 x) {
    float3 num = x * (2.51 * x + 0.03);
    float3 den = x * (2.43 * x + 0.59) + 0.14;
    return saturate(num / max(den, 1e-6));
}

// The sRGB OETF. The threshold is the ENCODE one (0.0031308), never the decode one.
float3 linearToSrgbEncode(float3 x) {
    float3 lo = 12.92 * x;
    float3 hi = 1.055 * pow(x, 1.0 / 2.4) - 0.055;
    return float3(x.r <= 0.0031308 ? lo.r : hi.r,
                  x.g <= 0.0031308 ? lo.g : hi.g,
                  x.b <= 0.0031308 ? lo.b : hi.b);
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float3 hdr = uSceneTex.Sample(uSceneSmp, uv).rgb;
    // +inf IS reachable from a large emissiveFactor and inf/inf in the ACES form is NaN. 65504 is the
    // largest finite half, so this clamp is a no-op for everything the RGBA16Float buffer can hold
    // except the infinity itself. max(0) additionally makes Reinhard's pole at x = -1 unreachable.
    // NaN is a DECLARED NON-CHECK: min/max with NaN are implementation-defined in HLSL.
    hdr = max(min(hdr * uExposure, 65504.0), 0.0);
    float3 mapped = (uCurve == 1) ? tonemapReinhard(hdr)
                  : (uCurve == 2) ? tonemapAcesApprox(hdr) : hdr;
    // The OETF is UNCONDITIONAL -- TonemapOperator::None means no CURVE, never no ENCODE (INV-4).
    // Alpha is written as LITERAL 1.0, never the sampled alpha: the editor's ImGui::Image
    // alpha-blends this texture over the panel background, and a sampled alpha of 0 would make the
    // whole viewport transparent (INV-6).
    return float4(linearToSrgbEncode(saturate(mapped)), 1.0);
}
