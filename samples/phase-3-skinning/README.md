# Phase 3 — Skinning (task 3.5.1)

GPU skinning made visible, and three firsts for this project: the first skinned vertex ever drawn
here, the first `.aeroskel` read end to end, and the first caller of
`render::ForwardRenderer::createMesh` — the mesh registry the cooked-mesh chain has been building
toward since 3.3.1.

## What it shows

- **Two copies of the same registered mesh**, side by side, drawn from one `MeshHandle` and one
  submesh. They differ in exactly one field: `MeshInstance::palette`.
- **The left copy (cool blue) is drawn with an EMPTY palette** and never moves. That is not a
  fallback and not a bug — a skinned mesh with no pose is the bind pose its vertices are authored
  in, so the draw path routes it through the *static* pipeline and the right picture costs nothing.
  Freezing it beside its animated twin is the whole point of the sample.
- **The right copy (warm orange) is posed every frame**: `bindPose` once at load, then a sine-phased
  rotation about Z composed onto every non-root joint's bind rotation, then `computeJointPalette`
  into a palette the draw borrows. The phase lag per joint is what turns four rotations into a
  travelling wave rather than a hinge.
- **One directional light plus flat ambient, and a slow orbit camera**, copied from
  `phase-3-materials`' proven constants — a moving eye is what makes a GGX highlight judgeable, and
  the highlight tracking the *bent* surface is how you can tell normals are being skinned too.

The sample logs the skeleton's joint counts, the `createMesh` wall time and a rolling
`computeJointPalette` mean, because the validation page's number-bearing rows read from here rather
than from feel.

Output is raw linear on an SDR target — task 3.6.3 owns tonemap and gamma — so absolute brightness
is not the thing to judge here, and there are no shadows until 3.6.2. Normals under skinning use the
palette's upper 3×3 with no per-joint inverse-transpose, which is exact for the rigid joints this rig
uses and skews only under non-uniform *joint* scale (the industry trade, recorded in
`shaders/scene_skinned.vert.hlsl`).

## How to run

```bash
cmake --build --preset macos-release
build/macos-release/samples/phase-3-skinning/aero_sample_phase3_skinning
```

Escape or the window close button quits. `AERO_SHADER_TOOLS=OFF` compiles a stub `main` that logs
and returns 1: the sample needs the cooked `scene.vert`/`scene.frag`/`scene_skinned.vert` at
runtime, which is what the `aero_shaders` build dependency provides.

**An optional `argv[1]` overrides the fixture directory.** The two file names inside it are fixed —
`arm.aeromesh` and `arm.aeroskel` — so pointing the sample at another cooked pair needs no second
argument:

```bash
build/macos-release/samples/phase-3-skinning/aero_sample_phase3_skinning /tmp/some-other-rig
```

That is what the validation page's cap-refusal row (a rig with more than
`render::MAX_SKINNING_JOINTS` palette slots, which the draw path refuses with one latched warning)
and its real-model row drive.

## Content provenance

`arm.gltf` is authored here, not downloaded, and the two cooked artifacts beside it come from it.

| Property | Value |
|---|---|
| Geometry | an 8-segment × 11-ring tube, radius `0.25`, running 2 units along +Y; ring `i` sits at `y = 0.2·i`, and both ends are capped with their own duplicated rim vertices so the cap normals are axial |
| Counts | 106 vertices, 528 indices (176 triangles) |
| Attributes | `POSITION`, `NORMAL`, `JOINTS_0` (`u16×4`), `WEIGHTS_0` (`f32×4`) |
| Rig | five joints `J0`–`J4` in a plain chain at `y = 0, 0.4, 0.8, 1.2, 1.6`; `J0` is a **scene root**, so the cook's ancestor closure adds nothing and the cooked skeleton is exactly five records |
| Inverse bind matrices | `translation(0, −jointY, 0)` — the inverse of each joint's global bind transform, hand-derivable from the chain above |
| Weights | a linear blend between the two nearest joints, so `JOINTS_0` carries two live entries per vertex and `WEIGHTS_0` sums to 1 |
| Nodes | TRS-form exclusively, and one embedded base64 buffer, so the file is self-contained and its cook is deterministic |

The generator was a throwaway Python 3 script (standard library only) run once. It is deliberately
not committed: the `.gltf` bytes are the frozen ground truth, exactly as `tests/fixtures/assets/*`
are, and the table above is the specification it would be regenerated from.

### The cook commands

Run from the repo root with the release cooker. **One source asset, two cooks, one pinned GUID for
both** — `sourceGuid` means "the asset these bytes came from", which is the same meaning it carries
for a `.ktx2`, so a mesh and the skeleton of its skin share it rather than each inventing one.

```bash
build/macos-release/tools/cooker/aero_cooker mesh \
  --input  samples/phase-3-skinning/arm.gltf \
  --output samples/phase-3-skinning/arm.aeromesh \
  --guid   351a0000000000000000000000000001        # 7216 bytes

build/macos-release/tools/cooker/aero_cooker skeleton \
  --input  samples/phase-3-skinning/arm.gltf \
  --output samples/phase-3-skinning/arm.aeroskel \
  --guid   351a0000000000000000000000000001        # 704 bytes
```

`arm.aeroskel`'s 704 bytes are `64 + 128 × 5` exactly, which is the format's whole size rule and the
fastest way to confirm the closure added nothing.

### Regenerating

Re-run both commands after any deliberate cook change. **Any other time the output must be
byte-identical** — that is task 3.3.3's guarantee, the same one
`tests/cooker/determinism.sha256`'s header describes, and a difference means the cook changed and
owes a `COOKED_MESH_COOKER_VERSION` / `COOKED_SKELETON_COOKER_VERSION` bump. These two artifacts are
**not** in that frozen manifest, so nothing fails automatically if they drift; re-cook into a scratch
directory and `cmp` when in doubt. Both were checked that way the day they were committed.

## Known edges

- **A palette longer than `render::MAX_SKINNING_JOINTS` (85) skips the instance**, with one latched
  warning naming the cap and the storage-buffer unlock, rather than being truncated: a truncated
  palette binds the tail's vertices to joint 0 and reads as a modelling defect instead of a limit.
- **Only submesh 0 is drawn.** The rig here has exactly one, and resolving a cooked submesh's
  `materialIndex` to a real material is the caller's job by design — the registry stores the number
  and never interprets it.
- **Nothing in a scene can name a mesh, a skeleton or a material yet**, so the `RenderView` is
  assembled by hand. Task 3.1.5 owns that seam; when it lands, this file becomes the short version.
