// Task E.1.1 — passthrough, zero resources (the triangle.frag.hlsl shape). LINEAR out: the HDR
// target is linear and 3.6.3's resolve is what encodes, so a line pushed as (1,1,1,1) reads 232/255
// under the default ACES operator and 255 under None.
float4 main(float4 color : TEXCOORD0) : SV_Target0 {
    return color;
}
