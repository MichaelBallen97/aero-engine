# Phase 3 — Materials (task 3.4.1)

The material system's proof, and three firsts for this project: the first cooked `.ktx2` consumed by
a GPU, the first `.aeromat` read end to end, and the first GUID-resolved asset reference — the Phase
5 pak-resolution shape in miniature (`.aeromat` guid → the artifact's `AeroSourceGuid` → a GPU
texture).

## What it shows

- **A 6×6 sphere grid.** Roughness increases **across** (left → right, 0 → 1); metallic increases
  **down** (back → front, 0 → 1). White base colour, no textures — the built-in 1×1 identity default
  textures are what these 36 spheres exercise, so both RGBA8 formats are covered by the grid while
  the two cubes cover the block-compressed families. One directional light plus a flat ambient, and a
  slow orbit camera, because a moving eye is what makes a GGX highlight judgeable.
- **The corner sphere is tinted.** Grid instance (0,0) carries `MeshInstance::color = {1.0, 0.4,
  0.4}` and nothing else does. That per-object tint keeps its exact pre-3.4.1 meaning: it multiplies
  `baseColorFactor.rgb` exactly once.
- **The mapped cube** (left, above the grid). All five texture slots bound, with its material loaded
  from the committed `mapped_cube.aeromat` through `engine::parseMaterial`.
- **The mask cube** (right, above the grid). `alphaMode: mask` with a cutout checker in the alpha
  channel, built in code rather than from a file, and double-sided — so its far faces are visible
  *through* the holes. That is the proof of the cull-none pipeline, not an artifact.

Metals show only analytic highlights against near-black: v1 has no IBL, which is a named unowned gap.
Output is raw linear on an SDR target — task 3.6.3 owns tonemap and gamma — so absolute brightness is
not the thing to judge here.

## How to run

```bash
cmake --build --preset macos-release
build/macos-release/samples/phase-3-materials/aero_sample_phase3_materials
```

Escape or the window close button quits. `AERO_SHADER_TOOLS=OFF` compiles a stub `main` that logs and
returns 1: the sample needs the cooked `scene.vert`/`scene.frag` at runtime, which is what the
`aero_shaders` build dependency provides.

## Fixture provenance

Six 32×32 RGBA8 PNGs and the six `.ktx2` cooked from them are committed here. The PNGs were written
by exact per-pixel integer formulas — never drawn by hand — so anyone can reproduce them. Below,
`c` is the column 0–31 and `r` is the row 0–31, top-left origin, and `round(x)` is round-half-up.

| File | Formula |
|---|---|
| `basecolor.png` | 8×8 checker: `on = ((c / 8) + (r / 8)) % 2 == 1`. On ⇒ `(236, 124, 40, 255)` (orange), off ⇒ `(245, 245, 245, 255)` (near-white). |
| `metallic_roughness.png` | glTF's channel packing: `R = 0`, `A = 255`; `G = 40` for `c < 16` and `220` otherwise (roughness: smooth left, rough right); `B = 0` for `r < 16` and `255` otherwise (metallic: dielectric top, metal bottom). |
| `normal.png` | Four cosine bumps, one centred in each 16×16 cell at `(cell·16 + 8)`. With `d` the distance from that centre, `h = 6.0 · 0.5 · (1 + cos(π·d / 6))` for `d < 6` and `0` beyond. Central differences `dx = (h(c+1,r) − h(c−1,r)) / 2` and `dy = (h(c,r+1) − h(c,r−1)) / 2` (coordinates clamped to the image), `n = normalize(−dx, −dy, 1)`, stored as `R = round(127.5·(n.x+1))`, `G`, `B` likewise, `A = 255`. |
| `occlusion.png` | Border ring: `d = min(c, r, 31−c, 31−r)`; `v = 255` when `d ≥ 8`, else `60 + round(195·d / 8)`; `R = G = B = v`, `A = 255`. |
| `emissive.png` | Black except rows 12–19, which are `(255, 96, 16, 255)` — a bright horizontal bar. |
| `mask_basecolor.png` | The same 8×8 checker predicate, in ALPHA: on-cells `(80, 200, 120, 255)`, off-cells `(80, 200, 120, 0)` — far to each side of the 0.5 cutoff. |

The generator was a throwaway Python 3 script (`struct` + `zlib` + `math`, standard library only) run
once. It is deliberately not committed: the PNG bytes are the frozen ground truth, exactly as
`tests/fixtures/assets/*.png` are, and the formulas above are the specification either would be
regenerated from.

### Pinned GUIDs

| Fixture | GUID |
|---|---|
| `basecolor` (mapped cube baseColor) | `341a0000000000000000000000000001` |
| `metallic_roughness` (mapped cube) | `341a0000000000000000000000000002` |
| `normal` (mapped cube) | `341a0000000000000000000000000003` |
| `occlusion` (mapped cube) | `341a0000000000000000000000000004` |
| `emissive` (mapped cube) | `341a0000000000000000000000000005` |
| `mask_basecolor` (mask cube) | `341a0000000000000000000000000006` |

`mapped_cube.aeromat` references GUIDs 1–5; the mask cube's material is built in code and resolves
GUID 6 directly. A GUID that resolves to no loaded texture is a hard startup error naming it — a
material referencing an asset that is not there is a broken project, not a styling choice.

### The cook commands

Run from the repo root with the release cooker. Each artifact is 32×32 with `levelCount 6`.

```bash
build/macos-release/tools/cooker/aero_cooker texture \
  --input  samples/phase-3-materials/textures/basecolor.png \
  --output samples/phase-3-materials/textures/basecolor.ktx2 \
  --format bc1 --srgb   --guid 341a0000000000000000000000000001        # 1088 bytes

build/macos-release/tools/cooker/aero_cooker texture \
  --input  samples/phase-3-materials/textures/metallic_roughness.png \
  --output samples/phase-3-materials/textures/metallic_roughness.ktx2 \
  --format bc1 --linear --guid 341a0000000000000000000000000002        # 1088 bytes

build/macos-release/tools/cooker/aero_cooker texture \
  --input  samples/phase-3-materials/textures/normal.png \
  --output samples/phase-3-materials/textures/normal.ktx2 \
  --format bc5 --linear --guid 341a0000000000000000000000000003        # 1808 bytes

build/macos-release/tools/cooker/aero_cooker texture \
  --input  samples/phase-3-materials/textures/occlusion.png \
  --output samples/phase-3-materials/textures/occlusion.ktx2 \
  --format bc4 --linear --guid 341a0000000000000000000000000004        # 1088 bytes

build/macos-release/tools/cooker/aero_cooker texture \
  --input  samples/phase-3-materials/textures/emissive.png \
  --output samples/phase-3-materials/textures/emissive.ktx2 \
  --format bc1 --srgb   --guid 341a0000000000000000000000000005        # 1088 bytes

build/macos-release/tools/cooker/aero_cooker texture \
  --input  samples/phase-3-materials/textures/mask_basecolor.png \
  --output samples/phase-3-materials/textures/mask_basecolor.ktx2 \
  --format bc3 --srgb   --guid 341a0000000000000000000000000006        # 1808 bytes
```

**Every command spells its format and its colour space explicitly, and none of them uses `auto`.**
That is deliberate: this fixture set is a format-coverage matrix, not a workflow demo. Between them
the six artifacts carry `VK_FORMAT_BC1_RGB_UNORM_BLOCK` (131), `BC1_RGB_SRGB_BLOCK` (132),
`BC3_SRGB_BLOCK` (138), `BC4_UNORM_BLOCK` (139) and `BC5_UNORM_BLOCK` (141), so every
block-compressed family this project cooks lands on a real GPU; the two RGBA8 formats ride the
renderer's built-in 1×1 default textures, which the grid exercises.

### Regenerating

Re-run the six commands after any deliberate cook change. **Any other time the output must be
byte-identical** — that is task 3.3.3's guarantee, the same one
`tests/cooker/determinism.sha256`'s header describes, and a difference means the cook changed and
owes a `COOKED_TEXTURE_COOKER_VERSION` bump. These six are not in that frozen manifest, so nothing
fails automatically if they drift; re-cook and `cmp` when in doubt.

`mapped_cube.aeromat` is canonical text emitted by `engine::writeMaterialText`, so it is a fixpoint:
parsing it and writing it back reproduces the file byte for byte. Hand-edits are fine — the parser is
strict and will name the offending key — but a hand-edited file may no longer be canonical until it
is round-tripped.

## Known edges

- **A cooked texture whose top level is not block-aligned is refused**, with one ERROR naming the
  artifact-level fact, rather than uploaded as something else. The committed 5×3 BC5 test golden is
  the worked example: a perfectly valid cooked artifact that no backend will create a texture for.
  Cook as `rgba8`, or resize the source. Every fixture here is 32×32, so none of them trip it.
- **Deleting a slot key from `mapped_cube.aeromat`** (say the whole `"normal"` object) is legal: the
  parser accepts the shorter document and that map reverts to its built-in identity default, which is
  the fastest way to see what each map contributes. Unknown keys are warned about and ignored, and
  are stripped by the next canonical write.
- **`uvSet` is stored but not honoured.** `MeshVertex` carries one UV set in v1, so a non-zero
  `uvSet` produces one latched warning and is otherwise ignored. The format keeps the field so a
  future import path is lossless.
