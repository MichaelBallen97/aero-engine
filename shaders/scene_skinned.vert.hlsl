// Task 3.5.1 — the SKINNED twin of scene.vert.hlsl. Same binding law (vertex uniform buffers b# ->
// space1; input location n <- TEXCOORDn), same VsOutput field for field, same tail: the fragment
// stage cannot tell which vertex shader fed it, which is exactly why scene.frag.hlsl is untouched
// and the two pipelines share it.
//
// Stream 0 is mesh.hpp's 48-byte MeshVertex at locations 0-3, byte-identical to the static VS.
// Stream 1 is mesh_pack.hpp's 32-byte SkinVertex at locations 4-5: a uint4 of joint indices (the
// wire format's own width — rhi::VertexFormat::Uint4 exists and UShort4 does not) and a float4 of
// weights. This is the tree's FIRST integer vertex attribute.
//
// The palette rides vertex uniform slot 1 as 255 float4 rows = 85 joints x 3 rows = 4080 bytes,
// always pushed WHOLE from a zeroed renderer-owned scratch. 85 is measured, not guessed: SDL's
// Vulkan backend binds every push-uniform descriptor with a fixed 4096-byte range, so a larger
// block would be silently invisible on Vulkan and fine on D3D12/Metal (skinning.hpp records the
// pinned-source lines).
struct VsInput {
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 tangent  : TEXCOORD2;  // xyz = tangent, w = glTF handedness (+1/-1), passed through
    float2 uv       : TEXCOORD3;
    uint4  joints   : TEXCOORD4;  // palette slots, one per influence
    float4 weights  : TEXCOORD5;  // matching blend weights; the importers normalize, nothing here does
};
struct VsOutput {
    float3 worldPos     : TEXCOORD0;
    float3 worldNormal  : TEXCOORD1;
    float4 worldTangent : TEXCOORD2;
    float2 uv           : TEXCOORD3;
    float3 color        : TEXCOORD4;
    float4 position     : SV_Position;
};
cbuffer PerObject : register(b0, space1) {  // byte-identical to scene.vert.hlsl's block (208 bytes)
    float4x4 uMvp;    // column-major (engine Mat4 upload is a straight no-transpose memcpy; mat4.hpp)
    float4x4 uModel;
    float4x4 uNormalMatrix;  // transpose(inverse(toMat3(model))) embedded in a Mat4 (upper-left 3x3 used)
    float3   uInstanceColor; // the per-object tint (MeshRenderer.color) — multiplies baseColorFactor.rgb
    float    _pad0;
};
cbuffer JointPalette : register(b1, space1) {  // 4080 bytes: 85 joints, three row-major float4 rows each
    float4 uPaletteRows[255];
};

#define MAX_SKINNING_JOINTS_ 85

VsOutput main(VsInput input) {
    VsOutput output_;  // trailing-underscore local: matches scene.vert.hlsl / cube.vert.hlsl

    // TRANSFORM THEN BLEND, the linear-blend-skinning definition. Each joint's skinning matrix is
    // three ROW-MAJOR rows (skinning_pack.hpp packs them; the affine fourth row (0,0,0,1) is dropped
    // and nothing here reconstructs it), so a matrix-vector product is three dots.
    float4 p4 = float4(input.position, 1.0);
    float3 posM = float3(0.0, 0.0, 0.0);
    float3 nrmM = float3(0.0, 0.0, 0.0);
    float3 tanM = float3(0.0, 0.0, 0.0);
    float  wsum = 0.0;

    [unroll]
    for (uint k = 0; k < 4; ++k) {
        float w = input.weights[k];
        wsum += w;
        uint j = input.joints[k];
        // Defensive clamp, not a policy: an index past the pushed block would read whatever the
        // uniform ring held. The cook and the adapter both validate indices, and the renderer
        // refuses a palette larger than the cap, so this can only fire on a hand-built artifact.
        if (j >= (uint)MAX_SKINNING_JOINTS_) {
            continue;
        }
        float4 r0 = uPaletteRows[(3u * j) + 0u];
        float4 r1 = uPaletteRows[(3u * j) + 1u];
        float4 r2 = uPaletteRows[(3u * j) + 2u];
        posM += w * float3(dot(r0, p4), dot(r1, p4), dot(r2, p4));
        // Normals and tangents use the palette's upper 3x3 with NO per-joint inverse transpose:
        // exact for the rigid and uniformly-scaled joints real rigs use, skewed under NON-UNIFORM
        // joint scale. That is the industry trade, stated rather than discovered. uNormalMatrix
        // still applies afterwards, so OBJECT-level non-uniform scale keeps its 1.4.1 correctness.
        nrmM += w * float3(dot(r0.xyz, input.normal), dot(r1.xyz, input.normal), dot(r2.xyz, input.normal));
        tanM += w * float3(dot(r0.xyz, input.tangent.xyz), dot(r1.xyz, input.tangent.xyz),
                           dot(r2.xyz, input.tangent.xyz));
    }

    // THE ALL-ZERO-WEIGHTS CONTRACT. The importers may hand a vertex four zero weights (an unrigged
    // vertex inside a skinned primitive), and such a vertex must keep its authored position rather
    // than collapse to the origin — a whole ring of a tube snapping to (0,0,0) is the loudest defect
    // this shader can produce. wsum > 0 selects the skinned result; otherwise everything passes
    // through exactly as the static VS would compute it.
    float3 posL = (wsum > 0.0) ? posM : input.position;
    float3 nrmL = (wsum > 0.0) ? nrmM : input.normal;
    float3 tanL = (wsum > 0.0) ? tanM : input.tangent.xyz;

    // ...and from here the scene.vert.hlsl tail, verbatim in meaning: MVP for clip space, model for
    // world position, the normal matrix's 3x3 for normal and tangent, tangent.w untouched.
    output_.position     = mul(uMvp, float4(posL, 1.0));
    output_.worldPos     = mul(uModel, float4(posL, 1.0)).xyz;
    output_.worldNormal  = mul((float3x3)uNormalMatrix, nrmL);
    output_.worldTangent = float4(mul((float3x3)uNormalMatrix, tanL), input.tangent.w);
    output_.uv           = input.uv;
    output_.color        = uInstanceColor;
    return output_;
}
