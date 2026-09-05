// Task E.2.1 -- the sky's vertex stage. The fullscreen.vert.hlsl triangle (three positions from
// SV_VertexID, ZERO vertex buffers and ZERO vertex attributes), but the varying is a WORLD-SPACE RAY
// rather than a UV.
// Binding law (0.4.3 F8): vertex uniform buffers b# -> space1.
cbuffer SkyCamera : register(b0, space1) {
    float4x4 uInvViewProj;   // inverse(proj * view); packed exactly as uLightViewProj is and consumed
                             // exactly as scene.frag.hlsl:86's mul(uLightViewProj, float4(...)) is
};

struct VsOutput {
    float3 ray : TEXCOORD0;      // far - near, UNNORMALISED; the fragment stage normalises
    float4 position : SV_Position;
};

VsOutput main(uint vertexId : SV_VertexID) {
    VsOutput output_;   // trailing-underscore local: triangle.vert.hlsl / fullscreen.vert.hlsl's own
    // (0,0) (2,0) (0,2) -- the standard vertex-id fullscreen triangle.
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    // uv (0,0) -> NDC (-1, +1): framebuffer TOP-LEFT, the convention SDL_GPU normalises to on all
    // three backends (fullscreen.vert.hlsl, verbatim).
    //
    // AND THE Y SIGN HERE IS SELF-CANCELLING -- MEASURED, not assumed, and the OPPOSITE of
    // fullscreen.vert.hlsl's situation. There the varying is a UV that ADDRESSES A TEXTURE, so a
    // wrong sign is the classic vertically-flipped picture; HERE the varying is derived from THIS
    // SAME `ndc`, so flipping the sign moves the vertex and its ray together and the screen-position
    // -> ray mapping is unchanged. Seeded `uv * float2(2,2) + float2(-1,-1)` and the whole SB battery
    // stayed green, 276 of 276. What IS observable, and what SB8/SB9/SB14 redden on (67 assertions),
    // is a sign flip applied to the RAY ALONE.
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    // ADR-005: clip z in [0, 1], 0 = near. For a FIXED z the unprojected point is affine in ndc under
    // any affine view and under BOTH projections -- the inverse projection's w depends on z alone
    // (perspective) or is 1 (ortho) -- so interpolating this DIFFERENCE across the triangle is exact.
    // That is why the ray is far - near and NEVER far - eye: CameraView::eyePosition is wrong under a
    // parallel projection (as it is in Unity), and this formula never reads it.
    //
    // SV_Position.w is 1.0 at all three vertices, so the hardware's perspective-correct interpolation
    // of `ray` reduces to a plain linear interpolation -- which is what makes the paragraph above a
    // statement about the PICTURE and not only about the three corners.
    float4 nearP = mul(uInvViewProj, float4(ndc, 0.0, 1.0));
    float4 farP  = mul(uInvViewProj, float4(ndc, 1.0, 1.0));
    output_.ray = farP.xyz / farP.w - nearP.xyz / nearP.w;
    // z = 0.0 is inert: depth test AND depth write are both off on this pipeline
    // (fullscreen.vert.hlsl's own note, for the same reason).
    output_.position = float4(ndc, 0.0, 1.0);
    return output_;
}
