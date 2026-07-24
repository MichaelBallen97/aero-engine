// Task 1.4.1 — lit-primitive vertex shader. Applies the MVP and passes world-space position/normal
// plus the per-object base color to the fragment stage. Resource-binding law (SDL_gpu.h contract,
// 0.4.3 F8): vertex uniform buffers b# -> space1 (matches cube.vert.hlsl).
// Vertex input semantics use TEXCOORDn (SDL_shadercross convention): input location n <- TEXCOORDn, so
// the pipeline's VertexAttribute.location {0,1} == {position, normal} (D9).
struct VsInput {
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
};
struct VsOutput {
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float3 color       : TEXCOORD2;
    float4 position    : SV_Position;
};
cbuffer PerObject : register(b0, space1) {
    float4x4 uMvp;    // column-major (engine Mat4 upload is a straight no-transpose memcpy; mat4.hpp)
    float4x4 uModel;
    float4x4 uNormalMatrix;  // transpose(inverse(toMat3(model))) embedded in a Mat4 (upper-left 3x3 used)
    float3   uBaseColor;
    float    _pad0;
};
VsOutput main(VsInput input) {
    VsOutput output_;  // trailing-underscore local: matches triangle.vert.hlsl / cube.vert.hlsl
    output_.position    = mul(uMvp, float4(input.position, 1.0));
    output_.worldPos    = mul(uModel, float4(input.position, 1.0)).xyz;
    output_.worldNormal = mul((float3x3)uNormalMatrix, input.normal);
    output_.color       = uBaseColor;
    return output_;
}
