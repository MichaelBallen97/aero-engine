// Task E.1.4 — the selection mask's fragment stage. Pairs with scene.vert.hlsl and
// scene_skinned.vert.hlsl UNCHANGED, which is what makes the mask pass's LessOrEqual depth test exact
// against the depth the forward pass wrote: same vertex stage, same 208-byte PerObject block, same
// bits. No graphics API guarantees position invariance across two DIFFERENT vertex shaders, and the
// failure mode of getting that wrong is a SPECKLED mask that reads as a depth-bias bug.
// Binding law (0.4.3 F8): fragment uniform buffers b# -> space3.
//
// THE FIVE INPUTS ARE DECLARED AND NONE IS READ, and that is load-bearing rather than sloppy: this
// signature is scene.frag.hlsl's entry point, character for character, so the pairing SPIRV-Cross
// emits for MSL matches the one the main pass already proves on three backends. Declaring a SUBSET is
// the class of change shadow_skinned.vert.hlsl's own comment warns about -- MSL attribute indices are
// assigned by DECLARATION ORDER.
//
// IT MUST NOT DISCARD. A Mask material's cut-away pixels are covered by this mask and the outline
// traces the quad -- the same gap shadow.frag.hlsl records for casters, with the same latched WARN on
// the C++ side. 8.2.1 owns alpha-tested passes.
cbuffer SelectionMask : register(b0, space3) {
    float  uMaskValue;   // SELECTION_MASK_SECONDARY (0.5) or SELECTION_MASK_PRIMARY (1.0)
    float3 _pad0;
};

float4 main(float3 worldPos : TEXCOORD0, float3 worldNormal : TEXCOORD1,
            float4 worldTangent : TEXCOORD2, float2 uv : TEXCOORD3,
            float3 color : TEXCOORD4) : SV_Target0 {
    // The target is R8Unorm, so only .r is written; the other three are the signature SDL wants.
    return float4(uMaskValue, 0.0, 0.0, 1.0);
}
