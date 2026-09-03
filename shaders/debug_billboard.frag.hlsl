// Task E.1.1 — one sampler at t0/s0, space2 (the binding law's fragment half). The renderer binds a
// built-in 1x1 WHITE texel when no atlas is set, so an untextured billboard is a solid quad of its
// own colour and the fragment stage never has an unbound slot (the Metal assertion 3.6.2 D7 records).
Texture2D    uAtlas    : register(t0, space2);
SamplerState uAtlasSmp : register(s0, space2);

float4 main(float2 uv : TEXCOORD0, float4 color : TEXCOORD1) : SV_Target0 {
    return uAtlas.Sample(uAtlasSmp, uv) * color;
}
