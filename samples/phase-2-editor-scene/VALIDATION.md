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
| macOS | ⏳ pending | — | — | — | — | — | — | — |
| Windows | ⏳ pending | — | — | — | — | — | — | — |
| Linux | ⏳ pending | — | — | — | — | — | — | — |

**One platform closes this gate.** Unlike Phase 0's fps floor and Phase 1's render rows, the claim here
is "the workflow produces a document", which is not platform-specific — the editor's three lanes are
already covered per-feature by `editor/VALIDATION.md`. Record the others if you run them; do not treat
them as blocking this artifact.

## What this does not close

Nothing in `editor/VALIDATION.md`. Those rows record that each *feature* works on each *OS* and are
still owed for Windows and Linux across all thirteen Phase 2 tasks, plus 2.6.2's row 1 (the dock-slot
row that FAILED on 2026-08-02 and needs re-confirming against PR #63's fix). This artifact records that
the *workflow* end-to-end produces a real document, which is a different claim.
