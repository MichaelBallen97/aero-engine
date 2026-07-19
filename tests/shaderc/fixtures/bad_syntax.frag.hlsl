// Test fixture: deliberately invalid HLSL syntax -- exit code 2, non-empty stderr, no output files
// (the `bad_syntax` ctest case, spec AC-6).
float4 main() : SV_Target0 {
    return this is not valid HLSL {{{
}
