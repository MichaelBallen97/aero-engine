// Task E.1.1 — the debug line renderer's vertex stage. The tree's FIRST consumer of a
// PrimitiveType::LineList pipeline and its FIRST consumer of VertexFormat::UByte4Norm.
// Binding law (0.4.3 F8): vertex uniform buffers b# -> space1; input location n <- TEXCOORDn.
// The colour arrives as a float4 because the attribute is UByte4Norm: the input assembler
// normalises, so the shader sees [0, 1] and never a 0..255 integer.
cbuffer DebugView : register(b0, space1) {
    float4x4 uViewProj;   // column-major; the engine's Mat4 upload is a straight no-transpose memcpy
};

struct VsInput {
    float3 position : TEXCOORD0;
    float4 color    : TEXCOORD1;
};

struct VsOutput {
    float4 color    : TEXCOORD0;
    float4 position : SV_Position;
};

VsOutput main(VsInput input) {
    VsOutput output_;   // trailing-underscore local: triangle.vert.hlsl / scene.vert.hlsl's own
    // World -> clip. NO CPU clipping anywhere: the hardware clips homogeneous coordinates, which is
    // the whole reason a world-space line needs none of selection_overlay.cpp's near-plane work.
    output_.position = mul(uViewProj, float4(input.position, 1.0));
    output_.color = input.color;
    return output_;
}
