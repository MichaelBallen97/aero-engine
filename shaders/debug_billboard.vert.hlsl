// Task E.1.1 — the camera-facing billboard's vertex stage. ALL FIVE INPUTS ARE DECLARED, IN THE
// PIPELINE'S ATTRIBUTE-TABLE ORDER: SPIRV-Cross numbers MSL [[attribute(n)]] by DECLARATION ORDER,
// so a stage that omits or reorders one binds Metal's attributes to the wrong data
// (shadow_skinned.vert.hlsl:11-17 records the same trap).
// Binding law (0.4.3 F8): vertex uniform buffers b# -> space1; input location n <- TEXCOORDn.
cbuffer DebugBillboardView : register(b0, space1) {
    float4x4 uViewProj;
    float2   uViewportPx;   // Frame::extent(), the DRAWN sub-rect in pixels
    float2   _pad0;
};

struct VsInput {
    float3 center : TEXCOORD0;
    float2 corner : TEXCOORD1;   // one of {-0.5, +0.5}^2; +y is the TOP of the sprite (NDC +y is up)
    float2 uv     : TEXCOORD2;
    float4 color  : TEXCOORD3;
    float  sizePx : TEXCOORD4;   // FULL width and height, in the frame's pixels
};

struct VsOutput {
    float2 uv       : TEXCOORD0;
    float4 color    : TEXCOORD1;
    float4 position : SV_Position;
};

VsOutput main(VsInput input) {
    VsOutput output_;
    float4 clip = mul(uViewProj, float4(input.center, 1.0));
    // SCREEN-CONSTANT SIZE, and this line is the whole point of the file (DD26 pins it as source
    // text). NDC spans 2 units across uViewportPx pixels, so sizePx pixels is sizePx * 2 /
    // uViewportPx in NDC; multiplying by clip.w makes the perspective divide return exactly that,
    // whatever the distance. Every vertex of one quad shares clip.w, so a billboard behind the eye
    // is clipped WHOLE rather than torn.
    clip.xy += input.corner * input.sizePx * 2.0 / uViewportPx * clip.w;
    output_.position = clip;
    output_.uv = input.uv;
    output_.color = input.color;
    return output_;
}
