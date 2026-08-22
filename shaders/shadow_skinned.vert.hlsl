// Task 3.6.2 — the SKINNED twin of shadow.vert.hlsl, and the reason a rigged model casts a shadow
// that matches what it draws. Position only: no normal, no tangent, no UV, no VsOutput.
//
// THE JOINT PALETTE BLOCK IS BYTE-IDENTICAL TO scene_skinned.vert.hlsl'S, and so is the skinning
// loop -- the same transform-then-blend, the same MAX_SKINNING_JOINTS_ guard placed BEFORE the
// weight sum, and the same all-zero-weights passthrough. That is not tidiness: if the two loops
// could disagree, a rigged model's CAST silhouette would differ from its DRAWN one, which looks
// like a bias bug and is not. 85 is measured, not guessed (skinning.hpp records the pinned SDL
// lines): SDL's Vulkan backend binds every push-uniform descriptor with a fixed 4096-byte range,
// so a larger block would be silently invisible on Vulkan and fine on D3D12/Metal.
// ALL SIX INPUTS ARE DECLARED, and the three this stage never reads are load-bearing rather than
// sloppy. SPIRV-Cross numbers MSL [[attribute(n)]] by DECLARATION ORDER, not by the HLSL semantic
// index -- so a stage declaring only TEXCOORD0/4/5 gets attribute(0)/(1)/(2), and the shared
// six-entry attribute table (which describes location 1 as the Float3 normal) would then hand Metal
// a Float3 where the shader wants a uint4. That is a hard pipeline-creation failure, not a silent
// one. Declaring the full stream keeps the indices aligned with scene_skinned.vert.hlsl's, which is
// what makes "the same vertex buffers bind unchanged for the depth pass and the main pass" true.
struct VsInput {
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;  // declared, never read -- see above
    float4 tangent  : TEXCOORD2;  // declared, never read
    float2 uv       : TEXCOORD3;  // declared, never read
    uint4  joints   : TEXCOORD4;  // palette slots, one per influence
    float4 weights  : TEXCOORD5;  // matching blend weights; the importers normalize, nothing here does
};
cbuffer PerObject : register(b0, space1) {
    float4x4 uLightMvp;
};
cbuffer JointPalette : register(b1, space1) {  // 4080 bytes: 85 joints, three row-major float4 rows each
    float4 uPaletteRows[255];
};

#define MAX_SKINNING_JOINTS_ 85

float4 main(VsInput input) : SV_Position {
    // TRANSFORM THEN BLEND, the linear-blend-skinning definition. Each joint's skinning matrix is
    // three ROW-MAJOR rows (skinning_pack.hpp packs them; the affine fourth row (0,0,0,1) is dropped
    // and nothing here reconstructs it), so a matrix-vector product is three dots.
    float4 p4 = float4(input.position, 1.0);
    float3 posM = float3(0.0, 0.0, 0.0);
    float  wsum = 0.0;

    [unroll]
    for (uint k = 0; k < 4; ++k) {
        float w = input.weights[k];
        uint j = input.joints[k];
        // Defensive clamp, not a policy. wsum accumulates AFTER this test, deliberately: it counts
        // influences actually APPLIED, so a vertex whose every index is out of range leaves wsum at
        // 0 and takes the passthrough below. Counting it before the test would give that vertex a
        // non-zero wsum over a zero posM and collapse it to the origin — the exact defect the
        // passthrough exists to prevent, and the one found by reading at 3.5.1.
        if (j >= (uint)MAX_SKINNING_JOINTS_) {
            continue;
        }
        wsum += w;
        float4 r0 = uPaletteRows[(3u * j) + 0u];
        float4 r1 = uPaletteRows[(3u * j) + 1u];
        float4 r2 = uPaletteRows[(3u * j) + 2u];
        posM += w * float3(dot(r0, p4), dot(r1, p4), dot(r2, p4));
    }

    // THE ALL-ZERO-WEIGHTS CONTRACT, identical to the main skinned VS: an unrigged vertex inside a
    // skinned primitive keeps its authored position rather than collapsing to the origin.
    float3 posL = (wsum > 0.0) ? posM : input.position;
    return mul(uLightMvp, float4(posL, 1.0));
}
