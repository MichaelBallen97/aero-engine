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
        scene.vert.hlsl      # task 1.4.1
        scene.frag.hlsl      # task 1.4.1
        scene_skinned.vert.hlsl  # task 3.5.1 -- the skinned twin; shares scene.frag.hlsl unchanged
        shadow.vert.hlsl         # task 3.6.2 -- the depth-only pass, static
        shadow_skinned.vert.hlsl # task 3.6.2 -- the depth-only pass, rigged
        shadow.frag.hlsl         # task 3.6.2 -- void main(); SDL requires a non-NULL fragment stage
        fullscreen.vert.hlsl     # task 3.6.3 -- the fullscreen pass scaffold (zero vertex buffers)
        tonemap.frag.hlsl        # task 3.6.3 -- exposure, tone curve, sRGB OETF
        debug_line.vert.hlsl      # task E.1.1 -- the first LineList pipeline's vertex stage
        debug_line.frag.hlsl      # task E.1.1 -- passthrough, zero resources
        debug_billboard.vert.hlsl # task E.1.1 -- the NDC * clip.w screen-constant offset
        debug_billboard.frag.hlsl # task E.1.1 -- one sampler; the default is a 1x1 white texel
        selection_mask.frag.hlsl    # task E.1.4 -- pairs with scene{,_skinned}.vert UNCHANGED
        selection_outline.frag.hlsl # task E.1.4 -- the 9-tap edge detect over the R8 mask
        sky.vert.hlsl               # task E.2.1 -- the fullscreen triangle, varying a world-space ray
        sky.frag.hlsl               # task E.2.1 -- the three-colour gradient; no mode, no branch
)
