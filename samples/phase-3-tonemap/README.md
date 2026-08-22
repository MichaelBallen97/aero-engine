# phase-3-tonemap — task 3.6.3's deliverable

The tonemap/gamma pass made visible and, more to the point, **measurable**. Every other sample in
this tree can only be judged by eye; this one prints the number a correct chain must produce and
lets a colour meter check it.

## What it draws, and why the layout is asymmetric

**Row A, across the top — eleven emissive-only patches, dark left to bright right.**
Their linear values are known *exactly*, not approximately. With `ambient = 0`, the directional
light's `intensity = 0`, no point lights, a black `baseColorFactor`, `metallicFactor = 0` and
emissive slot 4 left at its built-in white 1×1 default, `scene.frag.hlsl`'s whole shading reduces to

```
lit = emissive = uEmissiveFactor
```

so the HDR buffer holds the patch value and nothing else. `MaterialParams::emissiveFactor` is an
unclamped `Vec3` and nothing between the material and the shader clamps it, which is what lets the
ramp reach 16.0.

The patch values are `0, 0.01, 0.05, 0.18, 0.21404, 0.5, 1, 2, 4, 8, 16`. **0.18** is glTF's middle
grey. **0.21404** is the linear value the sRGB OETF maps to exactly one half — that patch is the
headline measurement.

**Row B, across the bottom — five lit spheres**, roughness 0.1 → 0.9 with the last two metallic,
under one directional light. A flat patch cannot answer "does a GGX highlight roll off correctly
now"; this row can.

**The asymmetry is deliberate.** The ramp is on top and runs dark-left to bright-right; the spheres
are below. A vertically flipped fullscreen triangle is therefore unambiguous at a glance — and it
has to be, because **no automated tier in this tree can see a flip**. This layout is its only
witness.

## Flags

| Flag | Default | Effect |
|---|---|---|
| `--tonemap=none\|reinhard\|aces` | `aces` | selects the tone curve |
| `--exposure=<float>` | `1.0` | parsed with `strtof`, then run through `sanitizeTonemapParams` |
| `--raw` | off | **bypasses `PostProcess` entirely** — draws straight into the swapchain exactly as every sample did before this task |

`--raw` is a branch on the whole render path, not a flag on the pass: with it, no `PostProcess` is
created at all. That is the point. **There is deliberately no way to disable the sRGB encode from
inside the pass** — `--tonemap=none` means no tone *curve*, never no *encode*. A flag that can be
set wrong is a way to ship the exact defect this task exists to remove, so the only escape hatch
lives at the call site.

## The headline: 55 versus 127

Run it twice at the **0.21404** patch and read the pixel with a colour meter:

```
aero_sample_phase3_tonemap --raw            # expect ~55 / 255
aero_sample_phase3_tonemap --tonemap=none   # expect ~127 / 255
```

Both draw the same linear value into the same 8-bit surface. `--raw` writes it unencoded, which is
what the picture has silently been doing since 3.4.1; `--tonemap=none` applies the sRGB OETF and
nothing else. The gap between those two readings **is** the bug this task closes. At glTF's middle
grey (0.18) the same pair reads ~46 and ~118.

The sample prints the whole comparison sheet at startup — all three operators *and* `--raw`, for
every patch, at this run's exposure — computed from `render::tonemapAndEncode` itself. **Compare a
screen reading against that printed table, not against a number copied out of a document**: the
table is what this build actually computes.

## What it exercises that nothing else does

The swapchain frame here **carries depth** (`RendererConfig{.depth = true}`), so this is the one
production caller resolving into a depth-carrying frame — `PostProcessConfig::outputDepthFormat`'s
non-`Invalid` arm. Both editor consumers make their output target depth-free and never reach it.

The per-frame line prints the HDR target's byte cost from `sceneTextureExtent()` — the
**allocation**, not the draw extent, because that is the memory actually held. At 1280×720 it reads
7.03 MB.

The exit lines report both latches. A live window drag can stretch exactly one frame and latch the
extent-mismatch warning once: the scene target's extent is carried one frame (the output frame's
real extent is only knowable after the swapchain image is acquired, and the cycle requires the scene
pass to be submitted before that acquire). That is the pass reporting honestly, not a defect.

## The standing caveat this resolves

Three sample READMEs in this tree carry a note that the engine's output is raw linear "until 3.6.3".
**That caveat is now resolved — by this task, exactly as its own text predicted.** Those READMEs are
deliberately left byte-identical: rewriting them would be a retroactive sweep, and each of the four
existing samples would need its own re-validation before its recorded pass could be claimed to
describe the new picture. Retrofitting them is a named, unowned follow-up.

## Running it

Needs `AERO_SHADER_TOOLS=ON` (the cooked `scene.{vert,frag}` plus `fullscreen.vert` /
`tonemap.frag`). Without them this compiles a stub `main` that logs and returns 1.

```bash
cmake --build --preset macos-debug --target aero_sample_phase3_tonemap
./build/macos-debug/samples/phase-3-tonemap/aero_sample_phase3_tonemap
```

Escape or the window close button quits. CI builds this on all three platforms as a compile-proof;
the visual pass is local — record it in `editor/validation/3.6.3-tonemap-gamma.md`.
