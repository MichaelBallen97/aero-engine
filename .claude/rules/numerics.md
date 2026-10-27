---
paths:
  - "engine/**/*.cpp"
  - "engine/**/*.hpp"
  - "editor/**/*.cpp"
  - "editor/**/*.hpp"
  - "tests/**/*.cpp"
  - "tests/**/*.hpp"
  - "samples/**/*.cpp"
---

# Floating point

Each rule here was earned by a defect, a green-and-wrong test or a measurement that contradicted a plan, and every one binds new work. The story behind each is in `docs/10-engineering-log.md`; grep it for the citation.

## Floating point

**`a + (b − a)` IS NOT `b` (E.1.3): an animation that must land on a boundary must HOLD its endpoint.** And
**`MAX_PITCH` is now exactly `HALF_PI`** — safe here because the composition is yaw-outer / pitch-inner, so
`right()` is independent of pitch, `viewMatrix()` has no `lookAt` and no up vector, and nothing divides by
`cos(pitch)`. Measured pole residual: worst component error 2.384e-07 over 1441 yaw samples; `right().y` is
exactly 0 there.

**`k * fl(1/255)` IS NOT `fl(k / 255)` (E.1.4)** — the reciprocal form is bit-unequal for **126 of the 256**
byte values, first at `k = 3`, and looks identical to six significant digits. **Any packer/unpacker pair that
wants an exact round trip must divide.**

**`fl(1e-4) * fl(1e-4)` IS NOT `fl(1e-8)` (E.2.2)** — one ulp below. A test arm claiming to evaluate "the same
expression the implementation evaluates" while spelling the squared constant as a literal is a **different**
computation that agrees only by rounding. Spell the constant squared.

**`std::clamp(NaN, lo, hi)` RETURNS NaN ON libc++ (3.7.2), AND `std::min(NaN, 48.0F)` RETURNS NaN (E.1.2).**
Every clamp on a value that can be non-finite needs an explicit finiteness arm *first*.

**A FLOOR DOES NOT MAKE A FUNCTION TOTAL WHEN THE NaN IS IN THE *DIFFERENCE* (E.2.2).** The spec's
`resolveSpotCone`, its header comment and the test that pinned it all agreed with one another and all three
were wrong: a NaN inner angle makes `cos(inner) - cos(outer)` NaN, the NaN takes the delta floor, and the
result is a finite **hard-edged cone at full intensity on the axis** — a plausible wrong picture. Test
`std::isfinite` on both cosines **FIRST** and return `{0, 0}`, which makes `saturate(cosAngle*0 + 0)^2`
exactly `+0.0` for every cosine, NaN included, on every backend (`std::cos(±inf)` is NaN, so infinite angles
land there too). **Generalisation: re-run every FORMULA in the precision it will execute in, exactly as
E.2.1's lesson was to re-run every GREP. A spec's own code is a claim, not a proof.**

**THREE FLOAT FACTS FROM E.1.2, EACH MEASURED AND EACH COUNTER-INTUITIVE.** (1) **A float-indexed lattice loop
does not terminate**: at `focus = 1e6, spacing = 0.01` the quotient reaches ~1e8 where `k += 1.0F` is a no-op
— and the naive repair traps, because `inf - inf` is NaN. Use an integer counter and clamp the span.
(2) **Decade lattices NEST in float32, so coordinate divisibility cannot identify a cadence.** (3)
**`pow10(n)` must be computed FRESH from `1.0F`**, never as a running product.
