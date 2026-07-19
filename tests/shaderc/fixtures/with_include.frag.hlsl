// Test fixture: --include forwarding (the `include_dir` ctest case). Requires --include pointing
// at tests/shaderc/fixtures/inc, exactly as SDL_shadercross's HLSL_Info::include_dir (a single path).
#include "common.hlsli"

float4 main() : SV_Target0 {
    return commonColor();
}
