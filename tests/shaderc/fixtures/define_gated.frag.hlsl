// Test fixture: --define forwarding (the `defines` ctest case). Self-verifying: compiling this file
// WITHOUT `--define AERO_TEST_COLOR=1` hard-fails at preprocessing, so a regression that silently
// drops --define forwarding cannot produce a false pass.
#if AERO_TEST_COLOR
float4 main() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
#else
#error AERO_TEST_COLOR must be defined (pass --define AERO_TEST_COLOR=1)
#endif
