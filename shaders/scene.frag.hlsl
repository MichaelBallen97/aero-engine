// Task 1.4.1 — lit-primitive fragment shader: ambient + one directional Lambert light + up to
// MAX_POINT_LIGHTS point lights with a cheap saturate(1 - d/range)^2 falloff. Binding law: this is the
// FIRST fragment *uniform buffer* in the tree — b# -> space3 (mirrors vertex UBOs at space1 and
// fragment textures/samplers at space2, per the SDL_gpu.h contract, 0.4.3 F8). VERIFY-AT-IMPLEMENTATION
// (plan Step 4): confirm this against the cooked scene.frag.json — a wrong space cooks and submits fine
// but renders wrong lighting.
#define MAX_POINT_LIGHTS 8
struct DirLight {
    float3 direction;
    float  intensity;
    float3 color;
    float  _pad0;
};
struct PointLight {
    float3 position;
    float  range;
    float3 color;
    float  intensity;
};
cbuffer Lights : register(b0, space3) {
    float3     uAmbient;
    uint       uPointCount;
    DirLight   uDir;
    PointLight uPoints[MAX_POINT_LIGHTS];
};
float4 main(float3 worldPos : TEXCOORD0, float3 worldNormal : TEXCOORD1, float3 color : TEXCOORD2) : SV_Target0 {
    float3 normal = normalize(worldNormal);
    float3 lit = uAmbient;
    lit += uDir.color * uDir.intensity * max(dot(normal, normalize(-uDir.direction)), 0.0);  // L toward light = -dir
    for (uint k = 0; k < uPointCount; ++k) {
        float3 toLight = uPoints[k].position - worldPos;
        float  dist = length(toLight);
        float  atten = saturate(1.0 - dist / max(uPoints[k].range, 1e-4));
        atten *= atten;
        lit += uPoints[k].color * uPoints[k].intensity * max(dot(normal, toLight / max(dist, 1e-4)), 0.0) * atten;
    }
    return float4(color * lit, 1.0);
}
