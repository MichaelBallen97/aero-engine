// Task 3.6.3 — the fullscreen pass scaffold's vertex stage. ZERO vertex buffers and ZERO vertex
// attributes: the three positions come from SV_VertexID (the triangle.vert.hlsl precedent, and
// rhi::GraphicsPipelineDesc documents both spans as "may be empty"). ONE oversized triangle, not two
// triangles: no shared edge, no diagonal seam, one fewer vertex.
// Binding law (0.4.3 F8): vertex uniform buffers b# -> space1.
// The pipeline uses CullMode::None, DELIBERATELY breaking the engine's Back convention -- a
// fullscreen triangle's winding is an artifact of the arithmetic below, not a modelling decision.
cbuffer FullscreenParams : register(b0, space1) {
    float2 uUvScale;    // drawExtent / textureExtent -- the source sub-rect (INV-2)
    float2 _pad0;
};

struct VsOutput {
    float2 uv : TEXCOORD0;
    float4 position : SV_Position;
};

VsOutput main(uint vertexId : SV_VertexID) {
    VsOutput output_;   // trailing-underscore local: triangle.vert.hlsl / scene.vert.hlsl's own
    // (0,0) (2,0) (0,2) -- the standard vertex-id fullscreen triangle.
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    // uv (0,0) -> NDC (-1, +1): framebuffer TOP-LEFT, which is the texture origin, and the
    // convention SDL_GPU normalises to on all three backends. Getting the Y sign wrong here is the
    // classic vertically-flipped picture and NO AUTOMATED TIER IN THIS TREE CAN SEE IT -- TM29 pins
    // the two float2 literals as source text and validation row 1 is the behavioural witness.
    output_.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    // SV_Position.z = 0.0 is the NEAR plane under ADR-005's [0,1] clip range. Depth test and write
    // are both off on this pipeline, so it is documentation rather than behaviour.
    output_.uv = uv * uUvScale;
    return output_;
}
