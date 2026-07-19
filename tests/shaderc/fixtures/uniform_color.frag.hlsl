// Reflection fixture: exactly one fragment-stage uniform buffer -> b0, space3 (the law).
cbuffer Color : register(b0, space3) {
    float4 uColor;
};

float4 main() : SV_Target0 {
    return uColor;
}
