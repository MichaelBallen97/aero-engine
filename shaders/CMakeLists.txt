# shaders/ — task 0.4.3: the engine's canonical, in-tree HLSL (docs/04). Compiled offline, at build
# time, into build/<preset>/shaders/ by tools/shaderc's aero_shaderc (cmake/shaders.cmake). These are
# the shaders the engine SHIPS -- test-only/deliberately-broken HLSL lives under tests/shaderc/fixtures
# instead (spec D7). 0.5.x is this pair's first consumer (visual correctness is that task's deliverable;
# compile correctness is this one's).
aero_add_shaders(aero_shaders
    SHADERS
        triangle.vert.hlsl
        triangle.frag.hlsl
        cube.vert.hlsl       # task 0.5.2
        cube.frag.hlsl       # task 0.5.2
)
