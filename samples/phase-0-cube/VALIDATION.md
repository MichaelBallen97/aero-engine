# Phase 0 gate ledger — `samples/phase-0-cube`

The Phase 0 deliverable gate (`docs/tasks/phase-0.md`) reads: *"Spinning textured cube @60fps on
mac/win/linux; CI green (ASan/UBSan) from commit #1."* CI proves the "builds and runs clean on all 3
OSes" half (it configures, builds, and unit-tests this sample's engine dependencies on macOS, Windows,
and Ubuntu on every push). It **cannot** prove the "@60fps, visible" half: CI has no vsync-capable
display on any lane (macOS/Windows CI runners have no attached monitor, and the Linux lane's GPU is
`lavapipe`, a software Vulkan rasterizer with no real refresh rate to lock to). The only way to prove the
60 fps gate is to run the actual visible sample on real hardware and watch it — a human, on-hardware
sign-off, recorded here per OS.

This file is that record. Task 0.5.3 lands the spin + fps-counter + objective-verdict code (see
`main.cpp` / `fps_gate.hpp`) and performs the macOS validation now; Windows and Linux validation is
tracked here as pending, closed later by a **code-free follow-up** that only fills in rows.

## How to validate one OS

1. **Build tools-ON** (default): `cmake --preset <os>-release && cmake --build --preset <os>-release`
   (needs `AERO_SHADER_TOOLS=ON` so the cube shaders are cooked).
2. **Run it**: launch `aero_sample_phase0_cube` from that preset's build tree. Watch the cube spin for
   at least ~5 seconds — it should look smooth, with the window title's fps number sitting near your
   display's refresh rate (e.g. ~60 on a 60 Hz panel).
3. **Drag-resize** the window: the cube's proportions must hold (no stretch), and the fps should stay
   steady once the drag settles.
4. **Press Escape** (or close the window). The sample prints an exit summary, including a line of the
   form:
   ```
   60fps gate: PASS — avg <X> fps, worst <Y> fps over <N> frames
   ```
5. **Record the row below**: date, machine/GPU, display + refresh rate, the reported avg/worst fps, and
   the verdict. A row counts as ✅ **only if both** the sample's own verdict is `PASS` **and** the run
   looked smooth to your eyes (a `PASS` verdict after a resize-heavy run may still warrant a clean
   re-run — see the note under the pass rule).

## Pass rule (restated from `fps_gate.hpp`)

The gate is a **lower bound**, not a strict "== 60" check, so it is honest on high-refresh displays: a
120/144 Hz panel that correctly runs faster than 60 under vsync still passes.

- `avg fps >= 58` (measured from raw present-to-present intervals, warm-up skipped, minimized gaps
  excluded)
- `worst-frame fps >= 30` (no single measured frame slower than ~33 ms — i.e., no dropped-vblank-class
  hitch)
- at least `120` measured frames (~2 s) before a verdict is trusted; fewer than that reads
  `INSUFFICIENT`, not a pass or fail

Exotic sub-60 Hz displays are out of scope for this gate.

## Validation table

| OS | Status | Date | Machine / GPU | Display / refresh | avg fps | worst fps | verdict | notes |
|----|--------|------|----------------|--------------------|---------|-----------|---------|-------|
| macOS | ✅ validated | 2026-07-19 | MacBook Pro (Apple M1 Pro) | Built-in / 60 Hz | 60.0 | 47.5 | PASS | Metal; clean ~29 s focused run (1719 frames), vsync-locked 60.0 fps. ProMotion panel validated at 60 Hz (also observed a correct ~120 fps lock at 120 Hz). |
| Windows | ⏳ pending | — | — | — | — | — | — | needs native run (D3D12) |
| Linux | ⏳ pending | — | — | — | — | — | — | needs native run (Vulkan; NOT lavapipe/CI) |

**Phase 0 gate status: OPEN** — closes only when all three rows above are ✅.
