// Task E.1.4 — the edge detect. THIS IS A TRANSCRIPTION OF selection_outline.hpp: the threshold below
// is the C++ SELECTION_MASK_PRIMARY_THRESHOLD, spelled as a bare literal so SO9's comment-stripped
// source-text pin can find it. Change one, change the other.
// Binding law: fragment textures/samplers -> space2, fragment uniform buffers -> space3.
//
// THIS PASS RUNS AFTER 3.6.3's OETF, so uPrimaryColor/uSecondaryColor are sRGB DISPLAY bytes, not
// linear. That is the OPPOSITE convention from DebugDraw's packDebugColor, deliberately: the outline
// is editor chrome and must not move with exposure or the tone curve.
cbuffer SelectionOutlineParams : register(b0, space3) {
    float4 uPrimaryColor;    // sRGB DISPLAY colour
    float4 uSecondaryColor;
    float2 uTexelStep;       // radiusPixels / textureExtent, per axis -- exact texel multiples
    float2 uUvMax;           // drawExtent / textureExtent -- the drawn sub-rect's far corner
};
Texture2D    uMaskTex : register(t0, space2);
SamplerState uMaskSmp : register(s0, space2);   // NEAREST, ClampToEdge -- a sample IS a texel

// D9: a tap that would leave the DRAWN rect reads the edge texel instead of the cleared margin, so an
// object continuing past the frame edge produces no false border there. A BEHAVIOURAL property, not a
// defensive clamp.
float tap(float2 uv) { return uMaskTex.Sample(uMaskSmp, clamp(uv, float2(0.0, 0.0), uUvMax)).r; }

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float c = tap(uv);
    float mn = c;
    float mx = c;
    // The eight box neighbours at EXACTLY +/- one step. Across an axis-aligned edge this makes the
    // band exactly 2*radius pixels wide -- radius INSIDE the silhouette and radius outside, MEASURED
    // at radius 1, 2, 4 and 8 rather than reasoned. Across a 45-degree edge the box reaches
    // radius*sqrt(2), which is the standard behaviour of a box neighbourhood and is documented,
    // not corrected.
    const float2 OFFSETS[8] = {
        float2(-1.0, -1.0), float2( 0.0, -1.0), float2( 1.0, -1.0),
        float2(-1.0,  0.0),                     float2( 1.0,  0.0),
        float2(-1.0,  1.0), float2( 0.0,  1.0), float2( 1.0,  1.0)
    };
    [unroll]
    for (int i = 0; i < 8; ++i) {
        float s = tap(uv + OFFSETS[i] * uTexelStep);
        mn = min(mn, s);
        mx = max(mx, s);
    }
    // THE WHOLE RULE. mn == mx covers the all-background case (both 0) and every interior pixel, so
    // no separate emptiness guard exists -- a redundant guard is a second place the rule can be
    // edited. The comparison is EXACT because the sampler is Nearest: a sample IS a texel value.
    if (mn >= mx) {
        return float4(0.0, 0.0, 0.0, 0.0);   // blended with SrcAlpha -> the resolved image, untouched
    }
    return (mx > 0.75) ? uPrimaryColor : uSecondaryColor;
}
