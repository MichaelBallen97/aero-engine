# phase-E-debug-draw — task E.1.1's deliverable

The engine's first world-space line renderer, made visible and — where it can be — **measurable**.
Every element in the scene is here to witness one claim; nothing is decoration.

## What it draws, and what each element proves

**A lit cube at the origin, a sphere beside it, a floor plane below.** The three built-in
primitives through `ForwardRenderer`, so the debug batch has real geometry to be depth-tested
against. No cooked asset, no committed fixture.

**A yellow wire box exactly on the cube (`DebugDepth::Tested`).** Its **far edges are hidden** by
the cube and its near edges are not — that is the depth test. It is also `CompareOp::LessOrEqual`
doing its job: the box's edges lie on the cube's own vertices, and under the engine's usual `Less`
convention a bit-identical depth would flicker rather than draw.

**A magenta circle inside the cube (`DebugDepth::Overlay`).** It shows **through** the solid cube,
which is the other half of the same claim and the mode E.2.3's always-visible gizmos need.

**A floor lattice whose alpha fades with distance.** Pushed through the `lines(span)` overload with
**per-vertex** colour, and each line is subdivided so the alpha varies *along* it as well as between
lines — an unsubdivided 12-unit line has both endpoints the same distance from the origin, so it
would fade between lines and not across them. That is the mechanism E.1.2's grid will use.

**Five white disc billboards marching away from the camera.** They keep the **same on-screen width**
at every depth. This is the one claim that cannot be judged from a still image, which is why the
camera dollies automatically (below).

**One disc behind the cube (`Tested` → hidden) and one at its centre (`Overlay` → visible).** The
billboard half of the depth claim.

**The three world axes from the origin**, X red / Y green / Z blue, `Overlay`.

**One pure white horizontal line across the upper third**, `Overlay`, away from everything else.
That is the colour meter's target and the **only exact numeric claim this sample makes**: it is
drawn with no lighting, no material and no interpolation, so the HDR buffer holds exactly `1.0`
there and a meter reading is the transfer chain's answer and nothing else.

## The camera dollies, on purpose

It sweeps between **6 and 14 world units** from the origin on a **6-second** period. The
screen-constant-size claim is about a *moving* camera; asking someone to drag a mouse while watching
five discs is asking them to judge two things at once.

## Flags

| Flag | Default | Effect |
|---|---|---|
| `--tonemap=none\|reinhard\|aces` | `aces` | selects the tone curve |
| `--exposure=<float>` | `1.0` | parsed with `strtof`, then run through `sanitizeTonemapParams` |
| `--raw` | off | **bypasses `PostProcess` entirely** — the batch draws straight into the swapchain frame, which carries depth. The A/B control. |
| `--overflow` | off | pushes exactly **1000 segments past the budget every frame**, so the drop count is steady and the budget WARN fires **once** |
| `--no-lines` | off | **skips `flush()` entirely.** Not "push an empty batch": an empty flush is already free, so that would measure nothing. This is the cost A/B. |
| `--grid` | off | **replaces the rotating content with the ground grid** (task E.1.2). Nothing else is drawn, so the grid is the only thing in the frame being judged. |

## --grid, and what to look for

The camera dollies between 6 and 14 units, which crosses the `level = -1 → 0` boundary at 10 — so
the crossfade is **visible in one cycle of the dolly the sample already performs**. What to watch:

* **No pop.** As the dolly crosses 10, no line changes brightness in one frame. Every world spacing
  keeps its exact alpha across the boundary by construction; a visible step means the weight rule was
  changed.
* **The printed cadence line**, which appears only when the decade changes. It names the view scale,
  the level, all three spacings, all three weights, all three radii, the emitted line count and the
  cap. **The count must stay under the cap.** Here it takes exactly two values, because within a
  decade the radii are a function of the spacings alone and the grid is centred on the fixed origin:

  | Level | View scale | Spacings | Radii | Emitted lines |
  |---|---|---|---|---|
  | `0` | above 10 | 1 / 10 / 100 | 24 / 90 / 90 | **1008** |
  | `-1` | below 10 | 0.1 / 1 / 10 | 2.4 / 24 / 90 | **1744** |

  against `DEBUG_GRID_MAX_LINES` = **2368**. Both are accountable rather than merely observed: with
  `R/s` an exact integer in every row above, a cadence of spacing `s` and radius `R` contributes
  `4 × (R/s − 1)` lines of 8 segments each — two families, a line every `s` out to the rim in each
  direction, minus the one at the origin the axes replace and the two at the rim whose length inside
  the disc is zero — plus 16 segments for the two axes. So level `0` is `736 + 256 + 0 + 16` and
  level `-1` is `736 + 736 + 256 + 16`.

  Both counts are *this sample's*, not the editor's: `Z_FAR` here is 100, so every radius is clamped
  to 0.9 × 100 = 90. That costs the coarsest cadence 480 segments at level `-1`, and at level `0` it
  empties that cadence outright — no multiple of 100 other than the origin falls inside a 90-unit
  disc, and the origin is where the axes go. The emitter's own reference figures — 2224 lines at the
  editor's default pose and 2304 at the worst pose over an adversarial sweep — are against the same
  bound, and are quoted in `debug_grid.hpp` beside the derivation of the cap.
* **The axes.** A red +X and a blue +Z through the origin, replacing the grid's own lines there
  rather than sitting on top of them.
* **The rim.** The grid fades out; it never ends at a visible edge. Every radius is clamped to
  0.9 × the far plane and the fade completes inside that, so there is no hard edge at the frustum.
* **Aliasing.** The lines are one pixel wide with no partial coverage, exactly like every other debug
  line in the tree — SDL_GPU exposes no line width on any backend. Shimmer at grazing angles is
  **expected**, not a defect; thick/AA lines remain E.1.1's standing unowned handoff.

## The startup table

Printed before the first frame, computed from `render::tonemapAndEncode` itself at *this run's*
exposure — never a number copied from a document. It carries two linear values:

* **1.00000** — the pure white line. The headline, and the row a meter reading is compared against.
* **0.25000** — the lattice line's own colour, so the fade has a number beside it. This one is a
  **reference, not an exact expectation**: a lattice line is alpha-blended over the lit floor, so a
  meter reading there is the composite, not the line's own value.

The allocated budget (after clamping) is printed above the table, and the last line names which
column *this* run will produce.

## Running it

```bash
cmake --build --preset macos-release -j3
./build/macos-release/samples/phase-E-debug-draw/aero_sample_phaseE_debug_draw
./build/macos-release/samples/phase-E-debug-draw/aero_sample_phaseE_debug_draw --raw
./build/macos-release/samples/phase-E-debug-draw/aero_sample_phaseE_debug_draw --overflow
./build/macos-release/samples/phase-E-debug-draw/aero_sample_phaseE_debug_draw --no-lines
./build/macos-release/samples/phase-E-debug-draw/aero_sample_phaseE_debug_draw --grid
```

Escape or the window close button exits. The per-frame line reports frame time, fps, the accepted
and dropped line counts, the billboard count, the draw-call count (0–4) and the HDR target's cost;
the exit line reports both latches, the flush count and the number of upload command buffers
acquired. Record the run in `editor/validation/E.1.1-debug-line-renderer.md`.

CI builds this on three OSes as a compile-proof only — there is no display there.

## Two caveats that are expected, not defects

**A 1-pixel line is half a point wide on a 2× display.** SDL_GPU exposes no line width at all: the
Vulkan backend hardcodes `lineWidth = 1.0f` and D3D12 sets `AntialiasedLineEnable = FALSE`. Thin,
slightly aliased lines are what this renderer produces by construction. Thick and anti-aliased lines
are a recorded handoff (they need quad expansion, which is a different vertex format and a different
pipeline), not an oversight.

**A line drawn exactly on a surface can still shimmer.** `LessOrEqual` wins on a *bit-identical*
depth, which is why the wire box on the cube is stable, but an interpolated depth across a large
polygon is not bit-identical to the line's own. The lattice is at `y = -1.0` over a floor plane at
`y = -1.01` for exactly this reason. E.1.2 owns the grid's real answer.
