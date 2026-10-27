---
paths:
  - "engine/render/**"
  - "engine/rhi/**"
  - "engine/scene_render/**"
  - "shaders/**"
  - "tests/render_*"
  - "tests/rhi_*"
  - "tests/scene_render*"
  - "tests/shader_loader_test.cpp"
  - "editor/src/viewport*"
  - "editor/src/selection_overlay*"
  - "editor/src/view_axis_gizmo*"
  - "editor/src/gizmo*"
  - "editor/src/picking*"
  - "editor/src/material_preview*"
  - "editor/include/aero/editor/viewport*"
  - "editor/include/aero/editor/selection_overlay*"
  - "editor/include/aero/editor/view_axis_gizmo*"
  - "editor/include/aero/editor/material_preview*"
  - "tests/editor/viewport*"
  - "tests/editor/selection_overlay*"
  - "tests/editor/view_axis_gizmo*"
  - "tests/editor/gizmo*"
  - "tests/editor/picking*"
  - "tests/editor/material_preview*"
---

# Rendering and the GPU

Each rule here was earned by a defect, a green-and-wrong test or a measurement that contradicted a plan, and every one binds new work. The story behind each is in `docs/10-engineering-log.md`; grep it for the citation.

## Four facts measured in the tree (Phase E)

**Four facts Phase E was built on, each measured in the tree and each contradicting a plausible guess.**
(1) The directional light **already** derives its direction from the entity's −Z world axis
(`scene_renderer.cpp:191-208`) — "it behaves like a point light" was an *affordance* gap, not a math bug,
because nothing drew the direction and nothing said which of two directional lights the bridge picked.
(2) **CLOSED BY E.5.1 (PR #114, `834bcb3`).** `buildRenderView`'s primitive arm used to never assign
`instance.material`, so a material dropped on a primitive was written, loaded by the ledger and then
discarded at draw time — reproduced rather than fixed by E.1.4, E.2.1, E.2.2, E.2.3 and E.2.4. Both
builders' primitive arms (`scene_renderer.cpp:193-202` and `:381-397`) now resolve through the ONE
`resolveMaterial`, whose second parameter is a fallback handle — `NO_SOURCE_MATERIAL` for a primitive — and
**both move together**, because the selection mask takes its cull mode from the resolved material.
(3) `rhi::PrimitiveType::LineList`
and `FillMode::Line` existed since 0.4.1 with **no consumer at all** until E.1.1, still the tree's only
`LineList` pipeline set — **`FillMode::Line` remains unexercised and wireframe-of-meshes is an unowned
handoff.**


## Rendering and the GPU

**A RENDER TARGET'S DEPTH IS ONLY READABLE IF IT WAS STORED, AND NOTHING CAN DETECT THAT IT WAS NOT.**
`RenderTargetConfig::depthStore` and `PostProcessConfig::sceneDepthStore` both default to `Clear` → `DontCare`.
On a tile-based deferred renderer — **every Apple Silicon Mac** — `DontCare` means the tile's depth is never
written back, so a later `LoadOp::Load` reads **GARBAGE, not stale-but-plausible values**: an EMPTY result on
Metal and a CORRECT one on D3D12/Vulkan, the worst failure shape there is. **And a pass attaching an existing
depth with `LoadOp::Load` must spell BOTH load ops**: the rhi cycles a depth target iff ANY load op is not
`Load`, `stencilLoadOp` defaults to `DontCare`, and the combination trips `SDL_BeginGPURenderPass`'s own
assertion and **HANGS the process** rather than failing it.

**A PASS THAT RE-RASTERISES GEOMETRY AND COMPARES AGAINST AN EXISTING DEPTH MUST PAIR WITH THE SAME VERTEX
STAGE, NOT AN EQUIVALENT ONE.** No graphics API guarantees position invariance across two different vertex
shaders; the failure mode is a **speckled mask that reads as a depth-bias bug**. E.1.4's mask pass reuses
`scene.vert` / `scene_skinned.vert` verbatim and pays `GpuPerObject`'s 208 bytes rather than a `Mat4`. It also
uses `CompareOp::LessOrEqual`, never `Less`: `Less` rejects every fragment whose depth is already in the
buffer, and the mask comes out empty.

**A PASS WHOSE TARGET HAS MARGIN MUST SET VIEWPORT AND SCISSOR, AND NO `quantum = 1` TEST CAN SEE THAT IT DID
NOT.** `renderShadowMap` sets neither, correctly — its texture has no margin. `renderSelectionMask`'s does:
with `beginRenderPass`'s default full-target viewport the mask maps across the ALLOCATION while the resolve
maps across the DRAWN rect, so the result is silently rescaled and slides as the panel is resized. Every
convenient test target is `quantum = 1`, where `drawExtent == textureExtent` and the bug is invisible; `OG7`
uses a 200x140 draw inside a 256x192 allocation and is the only case that can catch it.

**A UV THAT ADDRESSES A SUB-RECT NEEDS TWO DIFFERENT FAR BOUNDS (E.1.4).** `tonemapSourceUvMax` returns the
drawn rect's **exclusive** far edge — right for a fullscreen triangle's far corner, **wrong as a clamp**:
under Nearest filtering `floor((drawW / texW) · texW) == drawW` is the **first cleared MARGIN texel**. A clamp
bound is the last drawn texel's **centre**, `(drawExtent − 0.5) / textureExtent`, a DISTINCT quantity with its
own name. **The case that guards it must run on a MARGINED target** — `drawExtent == textureExtent` makes it
unobservable, because the hardware's own `ClampToEdge` answers correctly there.

**THE MASK MIRRORS `draw()`'s FRUSTUM CULL, WITH `draw()`'s OWN RESOLVED FRUSTUM (E.1.4).** "An off-screen
instance writes to no texel" is FALSE as a reason to skip it: `draw()` culls on the **cooked AABB**, so an
instance whose bounds are invalid or smaller than its triangles is dropped from the picture while still
projecting on screen. `draw()` publishes its resolved `(frustum, culling)` pair and `renderSelectionMask`
reads it — a mirror, never a second extraction. **Any future pass that re-draws a subset of the forward pass's
instances inherits this.**

**EVERY CLIP-SPACE PREDICATE IS PROJECTION-AWARE, AND THE PARAMETER IS NON-DEFAULTED (E.1.3).** An ortho
proj's bottom row is `(0,0,0,1)` and the view matrix is affine, so `clip.w` **does not depend on the world
point at all** — which made every "in front of the eye" test vacuous under ortho. `projectToViewport`,
`clipSegmentToNearPlane`, `gizmoOriginBehindCamera` and `viewportRay` all take a **NON-DEFAULTED**
`ProjectionMode` (`CLIP_Z_EPSILON = 1e-6` on `clip.z` in ortho). **The two gates are not equivalent and the
asymmetry is shipped**: perspective's `w > 0` means "in front of the EYE" and admits a point closer than
`nearPlane`; ortho's `z > 0` means "beyond the NEAR PLANE" and rejects it. **A universal `z`-based gate is
2.3.2's contract to change and is an unowned handoff.**

**A THRESHOLD CALIBRATED IN ONE PROJECTION'S DEPTH UNITS MEANS SOMETHING ELSE ENTIRELY IN THE OTHER (E.1.3,
and it shipped as a defect).** ImGuizmo's `0.001` near-band is calibrated against a **view-space** depth;
ortho's `clip.z` is a **normalised** one, so the same constant reached ~1.0 world units and suppressed the
gizmo outright after `focusOn`. The mirror is now gated on `Perspective` (PR #95). **"Stricter" is only safe
against the failure it was written for.**

**THE RHI GREW EXACTLY TWO CALLS AT E.1.1, AND EACH HAS ONE SENTENCE THAT VOIDS SILENTLY.**
`Device::recordBufferUpload` **replaces the WHOLE buffer** — everything past `data.size()` is undefined
afterwards, because the destination is cycled, which is why there is no `dstOffset` and why a partial write
cannot merge with last frame's contents. It refuses a command buffer with **a render pass open, in every
configuration** — that refusal is OURS, not SDL's, which only checks under `debug_mode`.
`Device::readbackTexture` is **BLOCKING and a test-and-tooling path**, and **its `SDL_WaitForGPUFences` call
is what performs the copy on D3D12**, where the download is deferred into `D3D12_INTERNAL_CleanCommandBuffer`.
Map before the wait and you read garbage **on Windows alone**. Release the fence **after** the wait.

**A DEPTH BIAS DOES NOTHING TO A LINE PRIMITIVE.** Measured at E.1.2 on the real `DebugDraw`: D3D12 and Metal
both exclude point and line primitives from rasterizer depth bias by specification, Vulkan permits without
guaranteeing. A 13 x 5 sweep moved a line at no gap down to `1e-5`; the same bias moved a `TriangleList`
billboard predictably, bracketing Metal's unit at `2^-24` for `D32Float`. **`DebugDrawConfig` therefore has no
bias field, and adding one for lines would be inert on two of three backends.** The remaining consumers are
triangle topologies: the shadow pass (`forward_renderer.cpp`, the only live consumer), and **E.2.3 EXAMINED
the handoff and DECLINED it** — every billboard it pushes is `DebugDepth::Overlay`, whose pipeline does not
test depth at all. **The handoff is RE-ISSUED** to whichever task first wants a depth-*tested* billboard.

**THE TESTED-LINE DEPTH NUDGE IS THE GROUND PLANE'S (E.5.2)** (`debugGridDepthNudge`, `z -= nudge * w`, set
every frame); a Tested producer off the ground needs its own rule. **`flush` pushes the line block PER DRAW** —
one shared push clips Overlay lines at the near plane, seen only by `DG24(b)`. **A roll-free camera gives the
ground ZERO screen-x depth gradient**: only a ROLLED pose tests the x term (`GR29(b)`), and the nudge is
independent of the viewport WIDTH when the aspect follows it (`GR30`). **A "bit-identical to the branch
point" validation row needs the grid off.**

**THE DEBUG BATCH IS SHARED, SO "THE BATCH IS EMPTY" IS A WHOLE-EDITOR CLAIM — AND THE WALL IS FOUR CASES, NOT
THREE.** Any task adding a producer to `render::DebugDraw` reddens every case that counts the batch exactly:
E.1.2's grid took `I108`/`I109`/`I111`; **E.2.3's icons took `I108`, `I109`, `I112`** (`I111` reads only LINE
quantities and icons are billboards; `I113` uses a `>` bound). **Fix it with the producer's own toggle seam,
never by restating the magnitude** — and `uploadCount()` is a *lifetime* counter, so for `I108`/`I109` the
toggle must precede the warm-up ticks while `I112` may take it after. **THE FOURTH CASE IS INVISIBLE TO THE
COUNTER GREP: `I110`'s roster subcase** walks `editor/src/*.cpp` and asserts exactly which files name
`DebugDraw` at all, so it reddens the moment a new producer TU lands — four commits before that producer has a
call site. **Sweep for the TYPE NAME as well as the accessors. The next producer inherits all four toggles.**

**THE MODES LIVE ONLY ON THE CPU, AND THE GPU RECEIVES DIFFERENCES — NEVER ENDPOINTS, NEVER A `lerp` (E.2.1).**
`resolveSkyGradient` packs `{horizon, sky - horizon, ground - horizon}` and `resolveAmbient` packs
`{mid, halfDelta}`, so **the shaders carry no mode, no branch and no selector**, and a zero delta makes
`x + 0*w` exact on every backend. **Verified against the Khronos registry, not assumed: `GLSL.std.450`'s
`FMix` is specified `x(1-a) + ya` and DXC maps `lerp` to it** (`docs/SPIR-V.rst:2595`), so a `lerp` form
differs from `x` by an ulp when `x == y` and would destroy the exactness that lets Solid + Flat reproduce a
clear and a constant **bit for bit**. **Do not "simplify" the resolvers back to endpoints.** (`-0.0 + 0.0*w`
is `+0.0` — equal, bit-different.)

**THE FULLSCREEN TRIANGLE IS BACK-FACING** under SDL_GPU's normalised convention, measured: `CullMode::Back`
on `SkyPass` deletes the sky entirely. `CullMode::None` there is load-bearing, not stylistic. **And a
depth-write-with-test-off seed is INERT on all three backends by API rule** — SDL Metal computes
`depthWriteEnabled = write && test` (`SDL_gpu_metal.m:1183`), Vulkan skips the depth update when
`depthTestEnable` is false, D3D12 maps test-off to `DepthEnable = FALSE`.

**A "NEGATED" COMPARISON IS ONLY NaN-SAFE IN THE DIRECTION IT WAS WRITTEN FOR (E.2.3).** 2.3.2's A10 is a
REFUSAL — `if (!(d <= radius)) return;` — and a NaN `d` correctly fires it. Translating that into an
ACCEPTANCE gives `d <= radius`; **`!(d > radius)`, which reads like the same thing, is its opposite** and
accepts a NaN. It shipped that way with a comment claiming the safety it did not have, and no case could see
it because `projectToViewport` refuses a non-finite projection.

**`MaterialParams{}` IS NOT THE RENDERER'S DEFAULT MATERIAL (E.1.4).** It defaults `metallicFactor` to glTF's
`1.0`, and *a metal with no environment to reflect renders near-black under analytic lights* — so a test quad
built from `MaterialParams{}` **is** drawn and is byte-identical to the background, which makes any colour
assertion over it vacuous. Start from `DEFAULT_MATERIAL_PARAMS`.

**THE TREE ASSERTS PIXELS NOW** — `RU5` first, then `DG5`–`DG16`, `DG18`, `I108`–`I113`, on Metal, WARP and
lavapipe, on every push. **But a pixel assertion on GENERATED geometry must not name a single pixel**: under a
centred camera an axis lands on a pixel *boundary*, where Metal lights row 95 while the projection names 96,
so `DG18` bounds where the lit **run** starts to **±1** — the width of the genuine fill-rule ambiguity. **A
later visual task that settles for "no backend error" is choosing to, not forced to**: `readbackTexture` plus
a `RenderTarget` under an identity camera makes "which pixel" arithmetic rather than judgement.

**A FLOAT COLOUR CONSTANT CAN COLLIDE WITH `std::numbers` (E.2.3).** `176/255` through the sRGB EOTF is
`0.43415`, 9.4e-05 from `std::numbers::log10e`, and trips `modernize-use-std-numbers` — as **every** accurate
spelling of that colour does; carry a `NOLINTNEXTLINE` with the reason beside it. **And re-derive such
literals rather than trusting a plan's**: E.2.3's plan spelled sRGB byte 32 as `0.0176`, which re-encodes to
**36**; the measured value is `0.0144`.


## Debug lines on HiDPI

**E.1.1's THICK-LINE HANDOFF IS FIRED, NOT CLEARED AND NOT DEFERRED.** E.2.3's macOS pass measured it: icons
scale exactly 2x and hold 22 points, but **387 of 441 sampled runs across the gizmo are ONE DEVICE PIXEL** —
0.5 points, half their apparent weight at 1x — and **that applies to every `DebugDraw` consumer, the grid and
world axes included.** It waited eight tasks to fire because HiDPI was not a validation row on any of them.
