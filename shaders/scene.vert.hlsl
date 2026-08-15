// Task 3.4.1 — PBR vertex stage (replacing 1.4.1's Lambert pair in place, per its own handoff).
// Applies the MVP and passes world-space position/normal/tangent, UV, and the per-object instance
// colour to the fragment stage. Binding law (SDL_gpu.h contract, 0.4.3 F8): vertex uniform buffers
// b# -> space1. Vertex input semantics use TEXCOORDn (SDL_shadercross convention): input location
// n <- TEXCOORDn, so VertexAttribute.location {0,1,2,3} == {position, normal, tangent, uv}
// (mesh.hpp's 48-byte MeshVertex).
struct VsInput {
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 tangent  : TEXCOORD2;  // xyz = tangent, w = glTF handedness (+1/-1), passed through
    float2 uv       : TEXCOORD3;
};
struct VsOutput {
    float3 worldPos     : TEXCOORD0;
    float3 worldNormal  : TEXCOORD1;
    float4 worldTangent : TEXCOORD2;
    float2 uv           : TEXCOORD3;
    float3 color        : TEXCOORD4;
    float4 position     : SV_Position;
};
cbuffer PerObject : register(b0, space1) {
    float4x4 uMvp;    // column-major (engine Mat4 upload is a straight no-transpose memcpy; mat4.hpp)
    float4x4 uModel;
    float4x4 uNormalMatrix;  // transpose(inverse(toMat3(model))) embedded in a Mat4 (upper-left 3x3 used)
    float3   uInstanceColor; // the per-object tint (MeshRenderer.color) — multiplies baseColorFactor.rgb
    float    _pad0;
};
VsOutput main(VsInput input) {
    VsOutput output_;  // trailing-underscore local: matches triangle.vert.hlsl / cube.vert.hlsl
    output_.position         = mul(uMvp, float4(input.position, 1.0));
    output_.worldPos         = mul(uModel, float4(input.position, 1.0)).xyz;
    output_.worldNormal      = mul((float3x3)uNormalMatrix, input.normal);
    // Tangents transform like normals under the transforms this engine composes (TRS; the normal
    // matrix's upper-left 3x3). w is handedness, not geometry — it passes through untouched.
    output_.worldTangent     = float4(mul((float3x3)uNormalMatrix, input.tangent.xyz), input.tangent.w);
    output_.uv               = input.uv;
    output_.color            = uInstanceColor;
    return output_;
}
