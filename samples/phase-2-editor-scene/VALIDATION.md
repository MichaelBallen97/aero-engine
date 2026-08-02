# Phase 2 gate ledger — `samples/phase-2-editor-scene`

The Phase 2 gate (`docs/tasks/phase-2.md`) reads: *"Create a project; create / move / edit / save
entities visually."*

**CI can prove none of it.** Every other gate in this repo has a mechanical half — Phase 0's sample
compiles, Phase 1's scene loads through `aero::scene_serialize` on all three lanes. This one does not,
and the reason is not a missing test tier: the gate's subject is *a person driving a GUI*, and its
whole claim is that the artifact beside it was **authored through the editor** rather than typed. A
hand-written `scene.json` is byte-identical to a real one. No script, no golden test, and no amount of
`AERO_REQUIRE_GPU` can tell the two apart — only the person who did it can attest to it. That is why
this ledger records provenance, not just pass/fail.

The committed scene's *format* is already pinned elsewhere and is not this file's job: `scene_golden_test.cpp`
(EG1–EG8) and `scene_serialize_test.cpp` (G1–G10) lock the schema, and `docs/09-file-formats.md` §2
specifies it.

## How to validate one OS

Follow `README.md`'s four steps in `aero_editor` on that OS, then record a row below. The steps in
brief:

1. `File ▸ New Project…` → `samples/phase-2-editor-scene`.
2. Build a scene with the panels alone — create, **parent**, gizmo-transform, edit a non-Transform
   reflected field, rename.
3. `File ▸ Save Scene` into `scenes/`.
4. `File ▸ New Scene`, then `File ▸ Open Scene` on that file — it must come back identical.

**Step 4 is the row that matters.** Saving proves the writer ran; only reopening proves the editor can
consume its own output, which is Epic 2.5's Definition of Done reached through the UI instead of
through a unit test.

## Validation table

| OS | Status | Date | Machine / GPU | Project created | Scene authored visually | Saved | Reopened identical | notes |
|----|--------|------|----------------|------------------|--------------------------|-------|--------------------|-------|
| macOS | ✅ validated | 2026-08-02 | MacBook Pro (Apple M1 Pro), Metal backend | PASS — `File ▸ New Project…` wrote `project.json` v1 + `assets/` + `scenes/` | PASS — see the provenance note below | PASS — `File ▸ Save Scene` → `scenes/Untitled.scene.json` | PASS — `File ▸ New Scene` then `File ▸ Open Scene`, scene returned identical | 4 entities, 8 components. Parent/child nesting is **not** exercised (all four entities are roots) — see "not exercised" below. |
| Windows | ⏳ not run | — | — | — | — | — | — | not blocking: one platform closes this gate |
| Linux | ⏳ not run | — | — | — | — | — | — | not blocking: one platform closes this gate |

### Provenance — why these bytes could not have been typed

This is the only evidence an artifact like this can offer, so it is recorded rather than asserted.
Every value below differs from what the code would produce on its own:

- **`DirectionalLight.intensity` = 9.1.** The component's default is `1.0f`
  (`engine/scene/include/aero/scene/light.hpp:28`). A reflected, non-Transform field driven through
  the Inspector — ADR-004's whole payoff, exercised end to end.
- **`Cube.position.z` = 3.9955873, `rotation.y` = 0.4782339** (≈57°). `seedDefaultScene` gives the
  Cube a default-constructed `Transform`. Values that are *near* a round number but not on it are what
  an ImGuizmo drag produces; typed input lands on the round number.
- **`OBJ2`** — a fourth entity that `seedDefaultScene` never creates, carrying a **renamed** id, a
  uniform `scale` of 3.06, `primitive` = 1 (not the default 0/Cube), and
  `color` = (0.12716262, 0.21443117, 0.7205882) — the non-round triple a colour-picker drag leaves
  behind.

Together those cover every verb in the gate sentence: **create a project** ✓, **create** ✓, **move** ✓,
**edit** ✓, **save** ✓ — plus rename, and the reopen that proves the document is real.

### Not exercised by this artifact

**Parent/child nesting.** All four entities are roots, so the scene never serializes a `parent` field
and this file does not stress the hierarchy path on load. That path is covered mechanically — the
`full` golden fixture in `tests/editor/` carries nested entities, and `scene_serialize_test.cpp`'s
G5/G6/G7 pin the structure — so nothing is unproven; it simply means **this** file is not the artifact
to reach for if a future task wants a hand-authored nesting case. Worth a deeper tree if the Phase 3
gate reuses it.

**One platform closes this gate.** Unlike Phase 0's fps floor and Phase 1's render rows, the claim here
is "the workflow produces a document", which is not platform-specific — the editor's three lanes are
already covered per-feature by `editor/VALIDATION.md`. Record the others if you run them; do not treat
them as blocking this artifact.

## What this does not close

Nothing in `editor/VALIDATION.md`. Those rows record that each *feature* works on each *OS* and are
still owed for Windows and Linux across all thirteen Phase 2 tasks, plus 2.6.2's row 1 (the dock-slot
row that FAILED on 2026-08-02 and needs re-confirming against PR #63's fix). This artifact records that
the *workflow* end-to-end produces a real document, which is a different claim.
