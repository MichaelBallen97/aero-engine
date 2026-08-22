# phase-3-shadows — task 3.6.2's deliverable

The first shadows this project has ever cast. A 30 × 30 ground plane under six casters, lit by one
directional light whose **elevation** sweeps 10°–80° on a 20-second triangle wave at a fixed azimuth
of 30°. The **camera is fixed** — the sun is the only thing moving, so shadow length and direction
are continuously checkable against it.

## The marked instances

| Colour | What it is | Why it is marked |
|---|---|---|
| **red** | a cube mirrored on X (`scale -1`), `doubleSided` | `transformAabb`'s absolute value, in the **light** frustum this time |
| **green** | its unmirrored twin, `doubleSided` | the comparison — both shadows must fall the same way |
| **orange** | a two-bone strip cooked **in memory**, waving on a 3 s cycle | the **only** path into the skinned depth pipeline |

The built-in primitive arm ignores a palette entirely, so a marked cube would witness the
`!palette.empty()` predicate and nothing about skinning. The rig is cooked rather than committed:
3.3.3 makes the cook deterministic cross-lane, so it is as stable as a golden and adds no fixture.

Two more things exist for exactly one validation row each and are **not** decoration: one **point
light** near the casters (row 5 — the shadowed region must stay lit by it), and a **procedural
normal map** on the ground (row 6 — with the built-in 1×1 flat normal default, comparing the
geometric normal against the mapped one is a no-op).

## Running it

```bash
cmake --build --preset macos-release -- -j 3
B=build/macos-release/samples/phase-3-shadows/aero_sample_phase3_shadows
$B                              # the default row: 2048 map, shadowDistance 40, sun sweeping 10-80
$B --no-shadows                 # the A/B twin (the cost arm)
$B --resolution 512             # row 8
$B --resolution 256             # row 8
$B --distance 12                # row 4 -- the region edge crosses the ground at z ~= -3.9
$B --elevation 15               # rows 2, 3, 6, 9 -- a FROZEN low sun, long shadows, reproducible
$B --elevation 75               # rows 2, 6 -- a frozen high sun, short shadows
```

**Arguments must be separate shell words.** `$B "--resolution 512"` — or an unquoted variable in
zsh, which does not word-split — hands the program **one** argument and the flag is silently
ignored. **Read the header line before trusting any number from a run**: it reports the requested
resolution *and* the allocated one, so a mistyped flag is visible immediately. Flags may be given in
any order and any combination; there is no positional argument, unlike `phase-3-culling`.

## The geometry, computed rather than discovered

```
fit radius r at --distance 40         = 42.96 world units
fit sphere centre                     = (0, -0.74, -4.88)
texelWorldSize = 2r / resolution      = 0.0420 (2048) · 0.1678 (512) · 0.3356 (256)
cubeL's shadow is ~1 world unit wide  = ~24 texels (2048) · ~6 (512) · ~3 (256)

distance from the fit centre, and the margin to r = 42.96:
  mirror (3, 1, 0) + half-diagonal          6.86    margin 36.1
  twin   (5, 1, 0) + half-diagonal          8.07    margin 34.9
  rig    (0, 1, 4) + reach                 10.6     margin 32.4
  ball   (0, 1.2, 0) + radius               6.06    margin 36.9
  cubeL  (-3, 1, 0) + half-diagonal         6.86    margin 36.1
  ground plane, worst corner (±15, 0, 15)  24.92    margin 18.0
  cubeL's longest shadow tip (elev 10°)     5.88    margin 37.1
```

**Every judged instance is inside the fit at every sun elevation in [0°, 90°]**, with at least 18.0
world units of margin at the worst point — and it is independent of the sun, because the fit follows
the **camera's** frustum and the camera is fixed. 3.6.1's row-6 failure mode (a marked instance
never inside the volume being judged) cannot occur here.

**The one exception, and it is `--distance 12`'s whole point:** the fit shrinks to `r = 12.87`
centred at `(0, 3.97, 8.30)`, and the region's edge crosses the ground plane at **z ≈ −3.94**, about
four units behind the casters. That is what makes the out-of-range boundary visible — but it means
the far half of the plane is legitimately unshadowed there, so **do not judge shadow presence at
`--distance 12`**.

## Known and expected

Output is **raw linear** on an SDR target — 3.6.3 owns tonemap and gamma, so judge relative
behaviour rather than absolute brightness. Point and spot lights cast nothing. Alpha-masked and
blended materials cast a **solid** silhouette, and the console says so once. One directional light
shadows. The shadow region is a **sphere** around the camera's frustum slice, so its edge on the
ground is a circle rather than a rectangle — at the default `--distance 40` that circle is far
outside the 30 × 30 plane and is not visible at all.
