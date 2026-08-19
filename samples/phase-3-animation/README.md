# Phase 3 — Clip playback (task 3.5.2)

Keyframe playback made visible, and three firsts for this project: the first `.aeroanim` read end to
end, the first caller of `render::sampleAnimation`, and the first sample to build a real `World` and
drive its picture from a component living in it.

## What it shows

- **Three copies of the same registered mesh**, side by side, drawn from one `MeshHandle` and one
  submesh. They differ in exactly two fields: `MeshInstance::palette` and their material.
- **The left copy (cool blue) is drawn with an EMPTY palette** and never moves. That is the
  deliverable, not a bug — a skinned mesh with no pose is the bind pose its vertices were authored
  in, so the draw path routes it through the *static* pipeline. It is the reference the other two are
  judged against.
- **The centre copy (warm orange) plays the clip at `speed = 1`, looping.** Watch the wrap: it
  returns to its first pose with no snap, because the clip's last key is bit-identical to its first.
- **The right copy (green) plays it at `speed = 0.25` and does NOT loop.** It moves at a quarter of
  the centre copy's rate — speed is a multiplier, countable against its neighbour — and after eight
  seconds it freezes on the clip's final pose and stays there. That freeze is the deliverable too.
  The pose it freezes in is the clip's *first* pose again, which is what makes the loop seamless, and
  that pose is visibly **not** the left copy's bind pose — which is how you can tell it is holding the
  clip's last frame rather than reverting to the rest pose.
- **The clip drives all three glTF paths through all three interpolation modes**, chosen so each is
  unmistakable at a glance:

  | joints | path | mode | what it looks like |
  |---|---|---|---|
  | `J1`–`J3` | rotation | `LINEAR` | the wave — three rotations 0.6 radians of phase apart |
  | `J4` (tip) | scale | `STEP` | a hard pop between scale 1 and 1.75 at each key, four times a cycle. **No interpolating sampler can produce a pop.** |
  | `J0` (root) | translation | `CUBICSPLINE` | a ±0.35 bob with visible ease-in and ease-out, from zero tangents at every key. **No linear sampler can produce a curve.** |

- **The World is real.** Each copy is an entity with a `Transform` — which is where its place in the
  row comes from — and the two driven copies also carry an `engine::AnimationPlayer`. Every frame the
  loop calls `advanceAnimationPlayer` on the component and reads `time` back **out of the World** to
  hand to `sampleAnimation`. Nothing in a scene can reference a mesh, a skeleton or a clip yet
  (task 3.1.5 owns that seam), so the `RenderView` is still assembled by hand — but the clock, the
  looping and the two speeds all live in ECS data.
- **One directional light plus flat ambient, and a slow orbit camera**, copied from
  `phase-3-materials`' proven constants.

The sample logs the three artifact sizes, the clip's channel count and duration, `bindAnimation`'s
four numbers and its one-off cost, and then a rolling `sampleAnimation` mean beside each player's
`time`, because the validation page's number-bearing rows read from here rather than from feel.

Output is raw linear on an SDR target — task 3.6.3 owns tonemap and gamma — so absolute brightness is
not the thing to judge here, and there are no shadows until 3.6.2. Normals under skinning use the
palette's upper 3×3 with no per-joint inverse-transpose, which is exact for the rigid joints this rig
uses and skews only under non-uniform *joint* scale; the `STEP` channel's scale is uniform, so this
rig has none (the industry trade, recorded in `shaders/scene_skinned.vert.hlsl`).

## How to run

```bash
cmake --build --preset macos-release
build/macos-release/samples/phase-3-animation/aero_sample_phase3_animation
```

Escape or the window close button quits. `AERO_SHADER_TOOLS=OFF` compiles a stub `main` that logs and
returns 1: the sample needs the cooked `scene.vert`/`scene.frag`/`scene_skinned.vert` at runtime,
which is what the `aero_shaders` build dependency provides.

**An optional `argv[1]` overrides the fixture directory.** The three file names inside it are fixed —
`wave.aeromesh`, `wave.aeroskel` and `wave.aeroanim` — so pointing the sample at another cooked rig
needs no further arguments; an optional `argv[2]` picks the submesh, for a model whose exporter split
it by material:

```bash
build/macos-release/samples/phase-3-animation/aero_sample_phase3_animation /tmp/some-other-rig 6
```

That is what the validation page's real-model row drives.

## Content provenance

`wave.gltf` is authored here, not downloaded, and the three cooked artifacts beside it come from it.
Its geometry and its rig are `phase-3-skinning/arm.gltf`'s recipe, regenerated from the table below
and byte-identical to that file's six geometry buffer views; what is new is the clip.

| Property | Value |
|---|---|
| Geometry | an 8-segment × 11-ring tube, radius `0.25`, running 2 units along +Y; ring `i` sits at `y = 0.2·i`, and both ends are capped with their own duplicated rim vertices so the cap normals are axial |
| Counts | 106 vertices, 528 indices (176 triangles) |
| Attributes | `POSITION`, `NORMAL`, `JOINTS_0` (`u16×4`), `WEIGHTS_0` (`f32×4`) |
| Rig | five joints `J0`–`J4` in a plain chain at `y = 0, 0.4, 0.8, 1.2, 1.6`; `J0` is a **scene root**, so the cook's ancestor closure adds nothing and the cooked skeleton is exactly five records |
| Inverse bind matrices | `translation(0, −jointY, 0)` — the inverse of each joint's global bind transform, hand-derivable from the chain above |
| Weights | a linear blend between the two nearest joints, so `JOINTS_0` carries two live entries per vertex and `WEIGHTS_0` sums to 1 |
| Nodes | TRS-form exclusively, and one embedded base64 buffer, so the file is self-contained and its cook is deterministic |
| Clip | one animation, `Wave`, 2 seconds, five channels: `LINEAR` rotation on `J1`, `J2` and `J3` (9 keys at `t = 0, 0.25 … 2.0`, a full sine period sampled by key **index** so key 8 is bit-identical to key 0); `STEP` scale on `J4` (5 keys at `t = 0, 0.4, 0.8, 1.2, 1.6`, alternating 1 and 1.75 and ending back at 1); `CUBICSPLINE` translation on `J0` (5 keys at `t = 0, 0.5 … 2.0`, `y` = 0, 0.35, 0, −0.35, 0, every in- and out-tangent zero) |
| Loop seam | every channel's last key equals its first exactly, the cubic tangents at both ends are zero, and the `STEP` track's last key lands **before** the clip ends and holds 1 through glTF's trailing clamp — so the wrap carries no discontinuity of its own and the tip's four pops all sit inside the cycle, on an even beat |

The generator was a throwaway Python 3 script (standard library only) run once. It is deliberately
not committed: the `.gltf` bytes are the frozen ground truth, exactly as `tests/fixtures/assets/*`
are, and the table above is the specification it would be regenerated from.

### The cook commands

Run from the repo root with the release cooker. **One source asset, three cooks, one pinned GUID for
all three** — `sourceGuid` means "the asset these bytes came from", so a mesh, the skeleton of its
skin and one of its clips share it rather than each inventing one. It is also what makes
`bindAnimation` report `sourceGuid matches` at load.

```bash
build/macos-release/tools/cooker/aero_cooker mesh \
  --input  samples/phase-3-animation/wave.gltf \
  --output samples/phase-3-animation/wave.aeromesh \
  --guid   352a0000000000000000000000000001            # 7216 bytes

build/macos-release/tools/cooker/aero_cooker skeleton \
  --input  samples/phase-3-animation/wave.gltf \
  --output samples/phase-3-animation/wave.aeroskel \
  --guid   352a0000000000000000000000000001 --skin 0   # 704 bytes

build/macos-release/tools/cooker/aero_cooker animation \
  --input  samples/phase-3-animation/wave.gltf \
  --output samples/phase-3-animation/wave.aeroanim \
  --guid   352a0000000000000000000000000001 --clip 0   # 1152 bytes
```

`wave.aeroskel`'s 704 bytes are `64 + 128 × 5` exactly, and `wave.aeroanim`'s 1152 are
`80 + 32 × 5 + 4 × 37 + 12 + 16 × 47` — the header, five channel records, 37 keys, the format's one
padding site and 47 values. Both are the fastest way to confirm the cooks took what they should.

### Regenerating

Re-run all three commands after any deliberate cook change. **Any other time the output must be
byte-identical** — that is task 3.3.3's guarantee, the same one `tests/cooker/determinism.sha256`'s
header describes, and a difference means the cook changed and owes a `COOKED_MESH_COOKER_VERSION` /
`COOKED_SKELETON_COOKER_VERSION` / `COOKED_ANIMATION_COOKER_VERSION` bump. These three artifacts are
**not** in that frozen manifest, so nothing fails automatically if they drift; re-cook into a scratch
directory and `cmp` when in doubt. All three were checked that way the day they were committed.

## Known edges

- **The left copy never animating, and the right copy freezing, are both the point.** Neither is a
  hang and neither is a missing feature.
- **A palette longer than `render::MAX_SKINNING_JOINTS` (85) skips the instance**, with one latched
  warning naming the cap and the storage-buffer unlock, rather than being truncated.
- **Only one submesh is drawn per instance.** The rig here has exactly one; `argv[2]` picks another
  for a model whose exporter split it by material.
- **A clip channel that names a node this rig does not have is skipped, not refused.** `bindAnimation`
  reports the count at load rather than failing, because glTF clips routinely animate camera and mesh
  nodes alongside joints. For this rig all five channels bind.
- **Nothing in a scene can name a mesh, a skeleton, a material or a clip yet**, so the `RenderView` is
  assembled by hand and the `AnimationPlayer` carries no clip reference. Task 3.1.5 owns that seam;
  when it lands, this file becomes the short version.
