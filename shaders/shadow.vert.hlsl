// Task 3.6.2 — the STATIC depth-only vertex stage. Position in, clip position out, nothing else:
// a depth pass shades nothing, so there is no VsOutput struct, no normal, no tangent and no UV.
// Binding law (0.4.3 F8): vertex uniform buffers b# -> space1; input location n <- TEXCOORDn.
//
// uLightMvp is lightViewProj * instance.model, composed ON THE CPU in the shadow loop -- the same
// shape scene.vert.hlsl's uMvp has, deliberately, rather than a second convention. The pipeline
// describes the full 48-byte MeshVertex stream at slot 0 (a layout may describe attributes the
// shader does not consume; the reverse is a pipeline-creation failure), so the same vertex buffer
// binds unchanged for the depth pass and the main pass.
struct VsInput {
    float3 position : TEXCOORD0;
};
cbuffer PerObject : register(b0, space1) {
    float4x4 uLightMvp;  // column-major (engine Mat4 upload is a straight no-transpose memcpy)
};
float4 main(VsInput input) : SV_Position {
    return mul(uLightMvp, float4(input.position, 1.0));
}
