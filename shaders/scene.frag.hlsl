// Task 3.4.1 — Cook-Torrance GGX metallic-roughness fragment stage (D8): height-correlated Smith
// visibility, Schlick Fresnel, Lambert diffuse, the 1.4.1 light set with its falloff UNCHANGED,
// ambient*AO diffuse-only, emissive added last, raw linear output (3.6.3 owns tonemap/gamma).
// Binding law (0.4.3 F8): fragment textures/samplers -> space2, fragment uniform buffers ->
// space3. TWO fragment UBOs is a tree first: Lights stays b0 (pushed once per view), the material
// block is b1 (pushed on material change) — pushFragmentUniforms slot == the b register index,
// verified against the cooked scene.frag.json (samplerCount 5, uniformBufferCount 2) before any
// visual judgment (the 1.4.1 space3 VERIFY, re-run for b1).
#define MAX_POINT_LIGHTS 8
static const float PI_ = 3.14159265359;
static const float ROUGHNESS_FLOOR = 0.045;  // alpha^2 degeneracy guard (D8, stated, standard)

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
cbuffer Lights : register(b0, space3) {           // 320 bytes (grew eyePosition, task 3.4.1)
    float3     uAmbient;
    uint       uPointCount;
    DirLight   uDir;
    PointLight uPoints[MAX_POINT_LIGHTS];
    float3     uEyePosition;
    float      _pad1;
};
cbuffer MaterialParams : register(b1, space3) {   // 48 bytes, pushed on material change
    float4 uBaseColorFactor;
    float3 uEmissiveFactor;
    float  uNormalScale;
    float  uMetallicFactor;
    float  uRoughnessFactor;
    float  uOcclusionStrength;
    float  uAlphaCutoff;                          // 0.0 for opaque (can never discard)
};
// The five glTF slots, declaration order == binding order t0..t4/s0..s4 (D7; the 0.4.3 contract).
Texture2D    uBaseColorTex : register(t0, space2);
SamplerState uBaseColorSmp : register(s0, space2);
Texture2D    uMetalRoughTex : register(t1, space2);
SamplerState uMetalRoughSmp : register(s1, space2);
Texture2D    uNormalTex : register(t2, space2);
SamplerState uNormalSmp : register(s2, space2);
Texture2D    uOcclusionTex : register(t3, space2);
SamplerState uOcclusionSmp : register(s3, space2);
Texture2D    uEmissiveTex : register(t4, space2);
SamplerState uEmissiveSmp : register(s4, space2);

// One BRDF evaluation for one light. N/V/L unit; lightColorIntensity is color * intensity *
// attenuation, premultiplied by the caller.
float3 shadeOneLight(float3 lightColorIntensity, float3 N, float3 V, float3 L,
                     float3 diffuseColor, float3 f0, float alpha) {
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float a2 = alpha * alpha;
    // GGX / Trowbridge-Reitz NDF
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    float D = a2 / max(PI_ * d * d, 1e-6);
    // Height-correlated Smith visibility (the G/(4 NdotL NdotV) form)
    float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    float Vis = 0.5 / max(lambdaV + lambdaL, 1e-6);
    // Schlick Fresnel
    float3 F = f0 + (1.0 - f0) * pow(1.0 - VdotH, 5.0);
    float3 specular = D * Vis * F;
    float3 diffuse = (1.0 - F) * diffuseColor / PI_;   // (1 - metallic) folded into diffuseColor
    return (diffuse + specular) * lightColorIntensity * NdotL;
}

float4 main(float3 worldPos : TEXCOORD0, float3 worldNormal : TEXCOORD1,
            float4 worldTangent : TEXCOORD2, float2 uv : TEXCOORD3,
            float3 color : TEXCOORD4) : SV_Target0 {
    // --- sample the five slots (absent slots carry the 1x1 identity defaults, D7) --------------
    float4 baseTexel = uBaseColorTex.Sample(uBaseColorSmp, uv);
    float4 baseColor = baseTexel * uBaseColorFactor * float4(color, 1.0);  // instance tint, once (AC-27)
    if (baseColor.a < uAlphaCutoff) {
        discard;  // Mask (AC-39); Opaque pushes cutoff 0.0, so this can never fire there
    }
    float2 mr = uMetalRoughTex.Sample(uMetalRoughSmp, uv).bg;  // glTF: metallic=B, roughness=G
    float metallic = mr.x * uMetallicFactor;
    float roughness = max(mr.y * uRoughnessFactor, ROUGHNESS_FLOOR);
    float ao = uOcclusionTex.Sample(uOcclusionSmp, uv).r;
    float occlusion = 1.0 + uOcclusionStrength * (ao - 1.0);   // glTF's lerp from 1
    float3 emissive = uEmissiveTex.Sample(uEmissiveSmp, uv).rgb * uEmissiveFactor;

    // --- TBN: Gram-Schmidt T against N, B from the handedness (D10; no fifth attribute) --------
    float3 N = normalize(worldNormal);
    float3 T = worldTangent.xyz - N * dot(N, worldTangent.xyz);
    T = normalize(T + float3(1e-6, 0.0, 0.0) * step(dot(T, T), 1e-10));  // degenerate-T guard
    float3 B = cross(N, T) * worldTangent.w;
    // Normal map: RG always, Z ALWAYS reconstructed (BC5 has only RG by format; RGBA8 and the
    // flat default carry Z but reconstruction reproduces it for unit-length inputs) — one path.
    float2 nxy = (uNormalTex.Sample(uNormalSmp, uv).rg * 2.0 - 1.0) * uNormalScale;
    float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
    N = normalize(nxy.x * T + nxy.y * B + nz * N);

    // --- BRDF inputs (D8) ----------------------------------------------------------------------
    float3 V = normalize(uEyePosition - worldPos);
    float alpha = roughness * roughness;                 // perceptual in, squared in the NDF
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor.rgb, metallic);
    float3 diffuseColor = baseColor.rgb * (1.0 - metallic);

    // --- lights: the exact 1.4.1 set, falloff byte-for-byte (AC-36) ----------------------------
    float3 lit = uAmbient * diffuseColor * occlusion;    // ambient*AO, diffuse only (D8)
    lit += shadeOneLight(uDir.color * uDir.intensity, N, V, normalize(-uDir.direction),
                         diffuseColor, f0, alpha);
    for (uint k = 0; k < uPointCount; ++k) {
        float3 toLight = uPoints[k].position - worldPos;
        float  dist = length(toLight);
        float  atten = saturate(1.0 - dist / max(uPoints[k].range, 1e-4));
        atten *= atten;
        lit += shadeOneLight(uPoints[k].color * uPoints[k].intensity * atten, N, V,
                             toLight / max(dist, 1e-4), diffuseColor, f0, alpha);
    }
    lit += emissive;                                      // last, unbounded (HDR-legal)
    return float4(lit, 1.0);                              // raw linear; the unorm target clamps
}
