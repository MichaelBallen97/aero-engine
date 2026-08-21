# phase-3-culling — the frustum-culling A/B rig (task 3.6.1)

An `N x N x N` grid of the renderer's three built-in primitives (cube / sphere / plane, cycling by
linear index), spaced 2 world units apart and centred on the origin. The eye is **fixed** just
outside the grid on +Z; only the **look direction** turns, a full 360 degrees every 12 seconds. So
the drawn fraction sweeps from its maximum down to zero and back once per cycle, and every instance
crosses a screen edge twice — an edge is where a culling bug shows, and a static camera never
visits one.

Nothing is committed here but this README, `main.cpp` and `CMakeLists.txt`: every mesh drawn is a
built-in primitive whose local box is **folded at `ForwardRenderer::create()` from the geometry
itself**, so there is no asset to load and no fixture to keep in step.

## Build and run

```bash
cmake --preset macos-debug            # or macos-release / windows-* / linux-*
cmake --build --preset macos-debug
./build/macos-debug/samples/phase-3-culling/aero_sample_phase3_culling
```

`Esc` or closing the window quits.

### The four runs the validation page reads

```bash
./build/macos-release/samples/phase-3-culling/aero_sample_phase3_culling                # default, N = 10 (1000)
./build/macos-release/samples/phase-3-culling/aero_sample_phase3_culling --no-cull      # the A/B twin
./build/macos-release/samples/phase-3-culling/aero_sample_phase3_culling 22             # stress, 10648
./build/macos-release/samples/phase-3-culling/aero_sample_phase3_culling 22 --no-cull   # stress A/B
```

Arguments may be given in any order. `--no-cull` sets `RenderView::cullingEnabled = false` for the
whole run; the first non-flag argument overrides `N`, clamped to `[2, 32]` (`32^3 = 32768` is the
stress ceiling).

## Reading the per-frame line

```
yaw  47.3  drawn   214 / 1000  culled   786 (78.6%)  record 0.412 ms
```

- **yaw** — the look direction's heading in degrees. `0` faces into the grid, `180` faces fully
  away. The three poses the validation pass records are `yaw ~ 0`, `~ 90` and `~ 180`.
- **drawn / culled** — `ForwardRenderer::lastFrameDrawn()` and `lastFrameCulled()`, read after the
  draw. They are **per-frame**, reset at the top of every `draw()`.
- **record** — the wall time of the `ForwardRenderer::draw(...)` call, in milliseconds.

**`record` is CPU record time, not "cull time".** The cull runs *inside* `draw()` and nothing
exposes it separately, by design — a timing accessor in the renderer would be API surface with one
consumer and a `steady_clock` read in the hot path. **The `--no-cull` A/B is what isolates the cull
term**: run the same grid, at the same `N`, and compare `record` at the same yaw. Culling on pays
one frustum extraction plus one box transform and up to six plane tests per instance, and saves the
whole per-instance record cost of everything it rejects.

`drawn + culled` equals the instance count here, but that is a property of this sample rather than a
guarantee: an instance skipped for a stale mesh handle, an out-of-range submesh or an over-cap
skinning palette is in **neither** bucket. Nothing in this grid can produce one.

## The colour key

| Colour | Instance | What it is for |
|---|---|---|
| grey | everything else | the bulk of the grid |
| **red** | the near-bottom-left corner, **mirrored** (scale `-1` on X) | a mirror is the fixture that separates a correct conservative box transform from one that drops the absolute value and yields an inside-out box, which the cull reads as "nothing to draw". It must cull and reappear exactly like its twin, and must never vanish while on screen. |
| **green** | its unmirrored neighbour | the twin to compare against |
| **blue** | the far top-right corner, carrying a one-entry **palette** | an instance with a palette is **exempt** from culling. Facing fully away (`yaw ~ 180`), the line reads `drawn 1` and it is the only thing left. It witnesses the `!palette.empty()` predicate, **not** skinning — the built-in primitive path ignores a palette entirely. |

The red/green pair share a `doubleSided` material, deliberately: a mirror flips triangle winding, and
without it back-face culling would make the red cube disappear for reasons that have nothing to do
with the frustum.

## Degenerate cameras

A projection that yields no usable frustum (`zNear == zFar`, a zero aspect, a collapsed view matrix)
**disables culling for that draw and warns once per renderer** — it never culls to black. This
sample's camera is always well-formed, so the closing line reports the warning "never fired"; the
validation page's degenerate-camera row makes a temporary two-line local edit here to see the other
answer, and reverts it afterwards.

## Profiling

The `*-release` presets build with `AERO_ENABLE_PROFILING=ON`, so `ForwardRenderer::draw` emits two
Tracy plots every frame: **`render.drawn`** and **`render.culled`**. They are the first production
plots in the tree. In a Tracy capture of a release run they should trace the yaw: `render.drawn`
falls to its floor as the camera turns away, `render.culled` rises to meet the instance count.
