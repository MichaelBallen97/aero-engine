// Task 0.5.2 — textured cube vertex shader (0.5.x's second seed, after the triangle). Applies the MVP
// from a uniform buffer to per-vertex geometry. Resource-binding law (SDL_gpu.h contract, 0.4.3 F8):
//   vertex: t/s -> space0, uniform buffers b# -> space1.
// Vertex input semantics use TEXCOORDn (SDL_shadercross convention): input location n <- TEXCOORDn, so
// the pipeline's VertexAttribute.location {0,1,2} == {position, uv, color}.
struct VsInput {
    float3 position : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float3 color    : TEXCOORD2;
};
struct VsOutput {
    float2 uv       : TEXCOORD0;
    float3 color    : TEXCOORD1;
    float4 position : SV_Position;
};
cbuffer Mvp : register(b0, space1) {
    float4x4 uMvp;   // column-major (engine Mat4 upload is a straight no-transpose memcpy; mat4.hpp)
};
VsOutput main(VsInput input) {
    VsOutput output_;                                    // trailing-underscore local: matches triangle.vert.hlsl
    output_.position = mul(uMvp, float4(input.position, 1.0));
    output_.uv = input.uv;
    output_.color = input.color;
    return output_;
}
