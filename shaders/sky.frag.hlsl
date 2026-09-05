// Task E.2.1 -- the gradient. THIS IS A TRANSCRIPTION OF engine/render/environment.hpp: the two
// exponents below are BARE LITERALS precisely so HE16's comment-stripped source-text pin can find
// them, and the C++ side (skyRadiance) is where the reasoning lives. Change one, change the other.
// Binding law: fragment uniform buffers -> space3. No texture, no sampler.
//
// THERE IS NO MODE HERE. Solid mode arrives as two ZERO deltas, and x + 0 * w is exact on every
// backend -- which is why the arithmetic below is written as additions of scaled deltas and NEVER as
// lerp(): DXC maps `lerp` to SPIR-V's FMix, specified as x*(1-a) + y*a, a different number from x
// when x == y.
cbuffer SkyParams : register(b0, space3) {
    float3 uHorizon;     float _pad0;
    float3 uSkyDelta;    float _pad1;   // skyColor - horizonColor
    float3 uGroundDelta; float _pad2;   // groundColor - horizonColor
};

float4 main(float3 ray : TEXCOORD0) : SV_Target0 {
    float y = normalize(ray).y;          // world up is +Y (ADR-005)
    float t = saturate(y);
    float b = saturate(-y);
    // Both weights are EXACTLY 0 at the horizon, so the two halves meet at uHorizon. pow(0, k) is
    // defined (0) on all three backends for k > 0, and the base is never negative.
    float wSky    = 1.0 - pow(1.0 - t, 4.0);
    float wGround = 1.0 - pow(1.0 - b, 8.0);
    float3 color = uHorizon + uSkyDelta * wSky + uGroundDelta * wGround;
    return float4(color, 1.0);           // raw linear HDR; 3.6.3 tonemaps it with everything else
}
