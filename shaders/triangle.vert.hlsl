// Task 0.4.3 fixture; 0.5.x's seed triangle. Zero resources by design (AC-5's zero-count case).
// Resource-binding law for ALL engine HLSL (SDL_gpu.h contract, spec 0.4.3 F8):
//   vertex:   t/s -> space0, uniform buffers b# -> space1
//   fragment: t/s -> space2, uniform buffers b# -> space3
struct VsOutput {
    float4 color : TEXCOORD0;
    float4 position : SV_Position;
};

VsOutput main(uint vertexId : SV_VertexID) {
    const float2 kPositions[3] = { float2(-0.5, -0.5), float2(0.5, -0.5), float2(0.0, 0.5) };
    const float4 kColors[3] = { float4(1, 0, 0, 1), float4(0, 1, 0, 1), float4(0, 0, 1, 1) };
    VsOutput output_;
    output_.position = float4(kPositions[vertexId], 0.0, 1.0);
    output_.color = kColors[vertexId];
    return output_;
}
